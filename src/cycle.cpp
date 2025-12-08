#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

static Simulator* simulator = nullptr;
static Cache* iCache = nullptr;
static Cache* dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;
static uint64_t dynRetired = 0;

// PC alw holds the address to be used by IF on the *next* cycle
static uint64_t PC = 0;

/*** Global stats we're responsible for ***/
static uint64_t loadUseStallCount = 0;    // tracks load-use stalls
static uint64_t loadBranchStallLeft = 0;  // tracks two-cycle load->branch stall window
static bool     squashNextIF = false;     // pending squash of speculative IF

// Cache stall counters (can overlap):
//  - I-cache miss: only IF stalls; older stages continue
//  - D-cache miss: MEM holds its instruction, WB continues, younger stages stall
// Stall counters represent cycles REMAINING after the current cycle
// When a miss occurs, we set this to missLatency
// Stall is ACTIVE when counter > 0 at the START of a cycle (before decrement)
static uint64_t iStallLeft = 0;  // remaining I‑cache stall cycles
static uint64_t dStallLeft = 0;  // remaining D‑cache stall cycles

// State to prevent double-counting I-cache hits on stall resume
static bool wasIStalled = false;
static bool stalledForHazard = false;

/** Create a micro‑architectural NOP with a given stage status */
Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013;  // addi x0,x0,0
    nop.isLegal = true;
    nop.isNop = true;
    nop.status = status;
    return nop;
}

static struct PipelineInfo {
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;

// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    cycleCount = 0;
    dynRetired = 0;
    PC = 0;
    loadUseStallCount = 0;
    loadBranchStallLeft = 0;
    iStallLeft = dStallLeft = 0;
    wasIStalled = false;
    stalledForHazard = false;
    squashNextIF = false;

    pipelineInfo.ifInst = nop(IDLE);
    pipelineInfo.idInst = nop(IDLE);
    pipelineInfo.exInst = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst = nop(IDLE);

    return SUCCESS;
}

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    Status status = SUCCESS;
    PipeState pipeState{};

    while (cycles == 0 || count < cycles) {

        // apply any pending squash of IF from prior taken branch (for visibility)
        pipeState.cycle = cycleCount;

        // snapshot current pipeline registers
        Simulator::Instruction oldIF = pipelineInfo.ifInst;
        Simulator::Instruction oldID = pipelineInfo.idInst;
        Simulator::Instruction oldEX = pipelineInfo.exInst;
        Simulator::Instruction oldMEM = pipelineInfo.memInst;
        Simulator::Instruction oldWB = pipelineInfo.wbInst;

        // new pipeline registers (initialized as idle NOPs)
        Simulator::Instruction newIF = nop(IDLE);
        Simulator::Instruction newID = nop(IDLE);
        Simulator::Instruction newEX = nop(IDLE);
        Simulator::Instruction newMEM = nop(IDLE);
        Simulator::Instruction newWB = nop(IDLE);

        bool branchTaken         = false;
        bool idIllegalException  = false;
        bool wbMemException      = false;

        bool applyDeferredSquash = squashNextIF;
        squashNextIF = false;

        // current stall state
        // D-cache: check BEFORE decrement (stall cycles 11-18 for miss at cycle 10)
        bool dStallActive = (dStallLeft > 0);

        // decrement counters for this cycle (all can count down together)
        if (iStallLeft > 0) iStallLeft--;
        if (dStallLeft > 0) dStallLeft--;
        if (loadBranchStallLeft > 0) loadBranchStallLeft--;

        // I-cache: check AFTER decrement (different timing)
        bool iStallActive = (iStallLeft > 0);

        // default next PC is fall‑through
        uint64_t nextPC = PC + 4;

        // --------------------------------------------------------
        // WB stage: MEM -> WB (WB alw continues)
        // During D-stall (from previous cycle), WB gets a bubble
        // --------------------------------------------------------
        if (dStallActive) {
            newWB = nop(BUBBLE);
        } else {
            newWB = simulator->simWB(oldMEM);
            if (newWB.isNop) newWB.status = oldMEM.status;

            if (!newWB.isNop && newWB.isLegal && !newWB.memException && newWB.dinCounted) {
                dynRetired++;
            }

            if (!newWB.isLegal || newWB.memException) {
                wbMemException = true;
                if (simulator) simulator->disableDinCounting();
                nextPC = newWB.nextPC;  // should be EXCEPTION_HANDLER (0x8000)
            }
        }

        // --------------------------------------------------------
        // Hazard Detection (Load-Use & Branch-Data)
        // Detect hazards EARLY so IF stage sees the correct stall signal
        // --------------------------------------------------------
        bool loadUseStall = false;
        bool loadUseEvent = false;  // count stats once per hazard
        bool branchDataStall = false;

        // Load-Use Hazard Detection
        if (!dStallActive) {
            // Load-Use
            if (!oldEX.isNop && oldEX.readsMem && !oldEX.writesMem && oldEX.rd != 0 && !oldID.isNop) {
                bool hazardOnRs1 = (oldID.readsRs1 && oldID.rs1 == oldEX.rd);
                // store data (rs2) forwarded WB->MEM; don't stall for that case
                bool hazardOnRs2 = (oldID.readsRs2 && oldID.rs2 == oldEX.rd && !oldID.writesMem);
                if (hazardOnRs1 || hazardOnRs2) {
                    loadUseStall = true;
                    loadUseEvent = true;
                }
            }

            // Branch-Data Hazard Check (Arith-Branch & Load-Branch)
            // Check hazards for instr currently in ID (oldID)
            if (!loadUseStall && !oldID.isNop) {
                bool isBranch = (oldID.opcode == OP_BRANCH);
                bool isJalr = (oldID.opcode == OP_JALR);
                if (isBranch || isJalr) {
                    if (!oldEX.isNop && oldEX.rd != 0) {
                        bool depRs1 = (oldID.readsRs1 && oldID.rs1 == oldEX.rd);
                        bool depRs2 = (oldID.readsRs2 && oldID.rs2 == oldEX.rd);
                        if (depRs1 || depRs2) {
                            if (oldEX.readsMem) {
                                branchDataStall = true;
                                if (loadBranchStallLeft == 0) {
                                    loadBranchStallLeft = 2;  // two-cycle stall window
                                    loadUseEvent = true;      // count once per hazard
                                }
                            } else if (oldEX.writesRd) {
                                branchDataStall = true;
                                if (loadBranchStallLeft == 0) loadBranchStallLeft = 1;
                            }
                        }
                    }
                }
            }
        }

        // Continue stall through remaining load->branch stall cycles
        if (loadBranchStallLeft > 0) branchDataStall = true;
        bool hazardStall = loadUseStall || branchDataStall;

        // --------------------------------------------------------
        // MEM stage: EX -> MEM + D‑cache access
        // --------------------------------------------------------
        bool dMissThisCycle = false;
        if (dStallActive) {
            // still waiting on previous D miss; hold MEM instruction
            newMEM = oldMEM;
        } else if (!oldEX.isNop && oldEX.status != SQUASHED) {
            Simulator::Instruction memInput = oldEX;

            // WB -> MEM forwarding for store data (load then store)
            if (memInput.writesMem && !oldWB.isNop && oldWB.writesRd && oldWB.rd == memInput.rs2 && oldWB.rd != 0) {
                memInput.op2Val = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
            }

            if (memInput.readsMem || memInput.writesMem) {
                memInput = simulator->simAddrGen(memInput);
                bool dHit = dCache->access(memInput.memAddress, memInput.writesMem ? CACHE_WRITE : CACHE_READ);
                if (!dHit) {
                    dStallLeft = dCache->config.missLatency;
                    dMissThisCycle = true;
                }
            }

            newMEM = simulator->simMEM(memInput);
            if (newMEM.isNop) newMEM.status = oldEX.status;
        } else {
            newMEM = nop(oldEX.status);
        }

        // --------------------------------------------------------
        // EX stage: ID -> EX with forwarding (unless stalling)
        // On D-stall from previous cycle, hold EX
        // On D-miss this cycle, still move ID->EX (miss cycle advances)
        // On hazard stall, insert bubble
        // --------------------------------------------------------
        if (dStallActive) {
            newEX = oldEX;  // freeze pipeline on D stall (detected or active)
        } else if (!oldID.isNop && !oldID.isLegal) {
            newEX = nop(SQUASHED);  // illegal squashed before EX
        } else if (hazardStall) {
            // bubble between ID and EX when stalling on load‑use or branch dependency
            newEX = nop(BUBBLE);
        } else {
            Simulator::Instruction exInput = oldID;

            // MEM -> EX forwarding (priority)
            if (!oldEX.isNop && oldEX.writesRd && oldEX.rd != 0) {
                uint64_t fwd = oldEX.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldEX.rd) exInput.op1Val = fwd;
                if (exInput.readsRs2 && exInput.rs2 == oldEX.rd) exInput.op2Val = fwd;
            }
            if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd != 0) {
                uint64_t fwd = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldMEM.rd && !(oldEX.writesRd && oldEX.rd == oldMEM.rd)) exInput.op1Val = fwd;
                if (exInput.readsRs2 && exInput.rs2 == oldMEM.rd && !(oldEX.writesRd && oldEX.rd == oldMEM.rd)) exInput.op2Val = fwd;
            }
            // WB -> EX forwarding (only if not alr forwarded from MEM)
            if (!oldWB.isNop && oldWB.writesRd && oldWB.rd != 0) {
                uint64_t fwd = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldWB.rd && !(oldEX.writesRd && oldEX.rd == oldWB.rd) && !(oldMEM.writesRd && oldMEM.rd == oldWB.rd)) exInput.op1Val = fwd;
                if (exInput.readsRs2 && exInput.rs2 == oldWB.rd && !(oldEX.writesRd && oldEX.rd == oldWB.rd) && !(oldMEM.writesRd && oldMEM.rd == oldWB.rd)) exInput.op2Val = fwd;
            }

            newEX = simulator->simEX(exInput);
            if (newEX.isNop) newEX.status = oldID.status;
        }

        // --------------------------------------------------------
        // ID stage: IF -> ID (unless stalled)
        // Branch hazards/resolution operate on the instruction *currently* in ID (oldID)
        // On D-stall from previous cycle with valid ID, hold ID
        // On D-miss this cycle, still move IF->ID (miss cycle advances)
        // --------------------------------------------------------
        Simulator::Instruction idEval = oldID;

        // Forwarding for branch resolution
        if (!dStallActive && !oldID.isNop) {
            if (!newMEM.isNop && newMEM.writesRd && newMEM.rd != 0) {
                uint64_t fwd = newMEM.readsMem ? newMEM.memResult : newMEM.arithResult;
                if (idEval.readsRs1 && idEval.rs1 == newMEM.rd) idEval.op1Val = fwd;
                if (idEval.readsRs2 && idEval.rs2 == newMEM.rd) idEval.op2Val = fwd;
            }
            if (!newWB.isNop && newWB.writesRd && newWB.rd != 0) {
                uint64_t fwd = newWB.readsMem ? newWB.memResult : newWB.arithResult;
                if (idEval.readsRs1 && idEval.rs1 == newWB.rd && !(newMEM.writesRd && newMEM.rd == newWB.rd)) idEval.op1Val = fwd;
                if (idEval.readsRs2 && idEval.rs2 == newWB.rd && !(newMEM.writesRd && newMEM.rd == newWB.rd)) idEval.op2Val = fwd;
            }
        }

        // Only trigger exception if not already squashing (from previous exception/branch)
        if (!applyDeferredSquash && !oldID.isNop && !oldID.isLegal) {
            idIllegalException = true;
            if (simulator) simulator->disableDinCounting();
            nextPC = oldID.nextPC;
        }

        bool idWillAdvance = !hazardStall && !dStallActive;
        if (idWillAdvance && !idIllegalException && !wbMemException && !oldID.isNop) {
            // branch detection uses opcode of instruction currently in ID
            bool isBranch = (idEval.opcode == OP_BRANCH);
            bool isJalr = (idEval.opcode == OP_JALR);
            bool isJal = (idEval.opcode == OP_JAL);
            if (isBranch || isJalr || isJal) {
                Simulator::Instruction resolved = simulator->simNextPCResolution(idEval);
                branchTaken = (resolved.nextPC != idEval.PC + 4);
                nextPC = resolved.nextPC;
            }
        }

        // What moves into ID
        // D-stall means ID is frozen so nothing new from IF
        if (dStallActive) {
            // D-stall from previous cycle: freeze ID (hold current instruction or bubble)
            newID = oldID;
        } else if (applyDeferredSquash) {
            newID = nop(SQUASHED);
        } else if (hazardStall) {
            // Hazard stall: hold ID
            newID = oldID;
        } else if (branchTaken && !idIllegalException && !wbMemException) {
            // Branch taken: squash the speculative instruction in IF
            // Don't set squashNextIF - the squash happens this cycle, not next
            newID = nop(SQUASHED);
            iStallLeft = 0;
        } else if (iStallActive) {
            // I-stall from previous cycle: bubble in ID
            newID = nop(BUBBLE);
        } else if (oldIF.isNop) {
            newID = nop(oldIF.status);
        } else {
            newID = simulator->simID(oldIF);

            bool illegalDetected = (!newID.isLegal) ||
                                   (newID.instruction == 0x00000000 && !newID.isNop && !newID.isHalt);
            if (illegalDetected) {
                if (newID.dinCounted) dynRetired++;
                idIllegalException = true;
                if (simulator) simulator->disableDinCounting();
                nextPC = newID.nextPC;
            }
            if (newID.isNop && newID.status == IDLE) newID.status = oldIF.status;
        }

        // --------------------------------------------------------
        // IF: Fetch
        // On D-stall from previous cycle, freeze IF (show same instruction)
        // On I-stall from previous cycle, show bubble
        // On D-miss this cycle, still try to fetch (miss cycle advances for IF)
        // On I-miss this cycle, show bubble
        // --------------------------------------------------------
        bool iMissThisCycle = false;

        if (wbMemException) {
            newIF = simulator->simIF(nextPC);
            newIF.status = NORMAL;
            PC = nextPC;
            iStallLeft = 0;
        } else if (idIllegalException) {
            // Illegal instruction detected during decode
            // IF shows the wrong-path PC but doesn't access cache
            // (The instruction will be squashed next cycle anyway)
            newIF = nop(BUBBLE);
            newIF.PC = PC;  // show where we would fetch
            squashNextIF = true;
            PC = nextPC;  // send pc to the handler for next cycle
        } else if (branchTaken) {
            // Branch taken: fetch from branch target
            // The squash of wrong-path instruction happens in ID section this cycle
            // After branch resolution, the fetch is no longer speculative
            std::cerr << "[ICACHE] cycle " << cycleCount << " branch access PC=0x" << std::hex << nextPC << std::dec << std::endl;
            bool iHit = iCache->access(nextPC, CACHE_READ);
            if (!iHit) {
                iStallLeft = iCache->config.missLatency;
                iMissThisCycle = true;
                newIF = nop(BUBBLE);
                newIF.PC = nextPC;
                PC = nextPC;  // hold at target until stall ends
            } else {
                newIF = simulator->simIF(nextPC);
                // Don't mark as SPECULATIVE - branch is already resolved
                PC = nextPC + 4;  // Advance past target
            }
        } else if (dStallActive) {
            // D-stall active so IF is frozen
            // Still might need to finish a pending I-cache fetch
            if (iStallActive) {
                // Both stalls active: show bubble at current PC
                newIF = nop(BUBBLE);
                newIF.PC = PC;
            } else if (oldIF.isNop) {
                // I-stall just ended during D-stall: cache block was loaded
                // during I-stall (access counted at I-miss time)
                // fetch instruction directly w/o re-accessing cache
                newIF = simulator->simIF(PC);
                PC = PC + 4;  // Advance for next fetch
            } else {
                // Already have an instruction in IF, hold it
                newIF = oldIF;
            }
        } else if (hazardStall) {
            // Hazard stall: IF frozen (keep same instruction)
            if (iStallActive) {
                newIF = nop(BUBBLE);
                newIF.PC = PC;
            } else {
                newIF = oldIF;
                if (branchDataStall) newIF.status = SPECULATIVE;
            }
        } else if (iStallActive) {
            // I-stall only: show bubble
            newIF = nop(BUBBLE);
            newIF.PC = PC;
        } else if (oldIF.isNop && oldIF.PC == PC && oldIF.status != IDLE) {
            // Just finished I-stall: instruction already fetched, don't re-access cache
            newIF = simulator->simIF(PC);
            PC = PC + 4;
        } else {
            std::cerr << "[ICACHE] cycle " << cycleCount << " normal access PC=0x" << std::hex << PC << std::dec << std::endl;
            // Normal fetch (or D-miss this cycle - still try to fetch)
            bool iHit = iCache->access(PC, CACHE_READ);
            if (!iHit) {
                iStallLeft = iCache->config.missLatency;
                iMissThisCycle = true;
                // Preserve IDLE status on first cycle, otherwise bubble
                if (oldIF.status == IDLE) {
                    newIF = nop(IDLE);
                } else {
                    newIF = nop(BUBBLE);
                }
                newIF.PC = PC;
            } else {
                newIF = simulator->simIF(PC);
                PC = PC + 4;
                // mark speculative if the new id instruction is a branch
                // that means we are guessing not taken
                bool newIdHasBranch = !newID.isNop && (newID.opcode == OP_BRANCH || newID.opcode == OP_JAL || newID.opcode == OP_JALR);
                if (newIdHasBranch) {
                    newIF.status = SPECULATIVE;
                }
            }
        }

        // --------------------------------------------------------
        // PC update:
        //  - Freeze on any stall (load-use, branch dep, i-miss, d-stall)
        //  - Otherwise: use nextPC from branch / exception logic
        // --------------------------------------------------------

        // exceptions from ID or WB squash younger instructions
        // ID-detected illegal: squash IF/ID only; allow older EX/MEM/WB to complete
        // WB-detected memory exception: squash IF/ID/EX/MEM (younger than faulting)
        if (wbMemException) {
            newID = nop(SQUASHED);
            newEX = nop(SQUASHED);
            newMEM = nop(SQUASHED);
        }

        // --------------------------------------------------------
        // Commit new pipeline registers
        // --------------------------------------------------------
        pipelineInfo.ifInst = newIF;
        pipelineInfo.idInst = newID;
        pipelineInfo.exInst = newEX;
        pipelineInfo.memInst = newMEM;
        pipelineInfo.wbInst = newWB;

        // --------------------------------------------------------
        // Halt detection: only when HALT reaches WB
        // --------------------------------------------------------
        if (newWB.isHalt) {
            // count any pending load-use event for this cycle
            if (loadUseEvent) {
                loadUseStallCount++;
            }

            // final cycle is accounted for in totals
            ++cycleCount;
            ++count;
            status = HALT;
            break;
        }

        // count load-use hazard events once per hazard (includes load->branch)
        if (loadUseEvent) {
            loadUseStallCount++;
        }

        (void)iMissThisCycle;
        ++cycleCount;
        ++count;
    }

    // ------------------------------------------------------------
    // Dump pipe state for the *last* simulated cycle
    // ------------------------------------------------------------
    pipeState.cycle = (cycleCount == 0) ? 0 : cycleCount - 1;
    pipeState.ifPC = pipelineInfo.ifInst.PC;
    pipeState.ifStatus = pipelineInfo.ifInst.status;
    pipeState.idInstr = pipelineInfo.idInst.instruction;
    pipeState.idStatus = pipelineInfo.idInst.status;
    pipeState.exInstr = pipelineInfo.exInst.instruction;
    pipeState.exStatus = pipelineInfo.exInst.status;
    pipeState.memInstr = pipelineInfo.memInst.instruction;
    pipeState.memStatus = pipelineInfo.memInst.status;
    pipeState.wbInstr = pipelineInfo.wbInst.instruction;
    pipeState.wbStatus = pipelineInfo.wbInst.status;
    dumpPipeState(pipeState, output);
    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT) break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    simulator->dumpRegMem(output);

    SimulationStats stats{};  // TODO incomplete implementation
    stats.dynamicInstructions = dynRetired;
    stats.totalCycles = cycleCount;

    // cache hit/miss counts from cache simulator
    stats.icHits = iCache ? iCache->getHits() : 0;
    stats.icMisses = iCache ? iCache->getMisses() : 0;
    stats.dcHits = dCache ? dCache->getHits() : 0;
    stats.dcMisses = dCache ? dCache->getMisses() : 0;

    // load-use stalls (includes load->branch use in this simple model)
    stats.loadUseStalls = loadUseStallCount;

    dumpSimStats(stats, output);
    return SUCCESS;
}
