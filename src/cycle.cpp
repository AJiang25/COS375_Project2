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

// PC alw holds the address to be used by IF on the *next* cycle
static uint64_t PC = 0;

/**TODO: Implement pipeline simulation for the RISCV machine in this file.
 * A basic template is provided below that doesn't account for any hazards.
 */

/*** Global stats we're responsible for ***/
static uint64_t loadUseStallCount = 0;    // tracks load-use stalls
static uint64_t loadBranchStallLeft = 0;  // tracks two-cycle load->branch stall window
static bool     squashNextIF = false;     // pending squash of speculative IF

// Cache stall counters (can overlap):
//  - I-cache miss: only IF stalls; older stages continue
//  - D-cache miss: MEM holds its instruction, WB continues, younger stages stall
static uint64_t iStallLeft = 0;  // remaining I‑cache stall cycles
static uint64_t dStallLeft = 0;  // remaining D‑cache stall cycles

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
    PC = 0;
    loadUseStallCount = 0;
    iStallLeft = dStallLeft = 0;

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
    pipeState.cycle = 0;

    while (cycles == 0 || count < cycles) {

        // apply any pending squash of IF from prior taken branch (for visibility)
        if (squashNextIF) {
            pipelineInfo.ifInst = nop(SQUASHED);
            squashNextIF = false;
        }

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
        bool branchDataStall     = false;

        // default next PC is fall‑through
        uint64_t nextPC = PC + 4;

        // current stall state
        bool iStallActive = (iStallLeft > 0);
        bool dStallActive = (dStallLeft > 0);

        // decrement counters for this cycle (all can count down together)
        if (iStallLeft > 0) iStallLeft--;
        if (dStallLeft > 0) dStallLeft--;
        if (loadBranchStallLeft > 0) loadBranchStallLeft--;

        // --------------------------------------------------------
        // WB stage: MEM -> WB (WB alw continues)
        // --------------------------------------------------------
        if (dStallActive) {
            newWB = nop(BUBBLE);
        } else {
            newWB = simulator->simWB(oldMEM);
            if (!newWB.isLegal || newWB.memException) {
                wbMemException = true;
                nextPC = newWB.nextPC;  // should be EXCEPTION_HANDLER (0x8000)
            }
        }

        // --------------------------------------------------------
        // detect load‑use hazard (EX load, ID uses rd) — only when pipe moves
        // --------------------------------------------------------
        bool loadUseStall = false;
        bool loadUseEvent = false;  // count stats once per hazard
        if (!dStallActive) {
            bool exIsLoad = (!oldEX.isNop && oldEX.readsMem && !oldEX.writesMem);
            if (exIsLoad && !oldID.isNop && oldEX.rd != 0) {
                if ((oldID.readsRs1 && oldID.rs1 == oldEX.rd) ||
                    (oldID.readsRs2 && oldID.rs2 == oldEX.rd)) {
                    loadUseStall = true;
                    loadUseEvent = true;
                }
            }
        }

        // --------------------------------------------------------
        // MEM stage: EX -> MEM + D‑cache access
        // --------------------------------------------------------
        bool dMissThisCycle = false;
        if (dStallActive) {
            // still waiting on previous D miss; hold MEM instruction
            newMEM = oldMEM;
        } else if (!oldEX.isNop) {
            // WB -> MEM forwarding for store data (load then store)
            if (oldEX.writesMem && !oldWB.isNop && oldWB.writesRd && oldWB.rd == oldEX.rs2) {
                oldEX.op2Val = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
            }

            // compute effective address before probing D-cache
            Simulator::Instruction memInput = oldEX;
            if (memInput.readsMem || memInput.writesMem) {
                memInput = simulator->simAddrGen(memInput);
                bool dHit = dCache->access(memInput.memAddress,
                                           memInput.writesMem ? CACHE_WRITE : CACHE_READ);
                if (!dHit) {
                    dStallLeft += dCache->config.missLatency;
                    dMissThisCycle = true;
                }
            }

            if (dMissThisCycle) {
                // keep instruction in MEM while stall counts down (with computed addr)
                newMEM = memInput;
            } else {
                newMEM = simulator->simMEM(memInput);
            }
        } else {
            newMEM = nop(BUBBLE);
        }

        // if MEM is stalled (prior or newly detected), WB gets a bubble
        if (dStallActive || dMissThisCycle) {
            newWB = nop(BUBBLE);
        }

        // --------------------------------------------------------
        // EX stage: ID -> EX with forwarding (unless stalling)
        // --------------------------------------------------------
        if (dStallActive) {
            newEX = oldEX;  // freeze pipeline on D stall
        } else if (!loadUseStall && !branchDataStall && loadBranchStallLeft == 0) {
            Simulator::Instruction exInput = oldID;

            // MEM -> EX forwarding (priority)
            if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd != 0) {
                uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldMEM.rd) {
                    exInput.op1Val = fwdVal;
                }
                if (exInput.readsRs2 && exInput.rs2 == oldMEM.rd) {
                    exInput.op2Val = fwdVal;
                }
            }

            // WB -> EX forwarding (only if not alr forwarded from MEM)
            if (!oldWB.isNop && oldWB.writesRd && oldWB.rd != 0) {
                uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldWB.rd &&
                    !(oldMEM.writesRd && oldMEM.rd == oldWB.rd)) {
                    exInput.op1Val = fwdVal;
                }
                if (exInput.readsRs2 && exInput.rs2 == oldWB.rd &&
                    !(oldMEM.writesRd && oldMEM.rd == oldWB.rd)) {
                    exInput.op2Val = fwdVal;
                }
            }

            newEX = simulator->simEX(exInput);
        } else {
            // bubble between ID and EX when stalling on load‑use or branch dependency
            newEX = nop(BUBBLE);
        }

        // --------------------------------------------------------
        // ID stage: IF -> ID (unless stalled)
        // Branch hazards/resolution operate on the instruction *currently* in ID (oldID)
        // --------------------------------------------------------
        Simulator::Instruction idDecodedNext = nop(BUBBLE);  // will hold decode of oldIF if we advance
        Simulator::Instruction idEval = oldID;                // instruction presently in ID

        if (dStallActive) {
            newID = oldID;  // freeze during D stall
        } else {
            // forward operands for instruction currently in ID (idEval)
            if (idEval.readsRs1 && idEval.rs1 != 0) {
                if (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd == idEval.rs1) {
                    idEval.op1Val = oldEX.arithResult;
                } else if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd == idEval.rs1) {
                    uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                    idEval.op1Val = fwdVal;
                } else if (!oldWB.isNop && oldWB.writesRd && oldWB.rd == idEval.rs1) {
                    uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                    idEval.op1Val = fwdVal;
                }
            }
            if (idEval.readsRs2 && idEval.rs2 != 0) {
                if (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd == idEval.rs2) {
                    idEval.op2Val = oldEX.arithResult;
                } else if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd == idEval.rs2) {
                    uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                    idEval.op2Val = fwdVal;
                } else if (!oldWB.isNop && oldWB.writesRd && oldWB.rd == idEval.rs2) {
                    uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                    idEval.op2Val = fwdVal;
                }
            }

            // branch detection uses opcode of instruction currently in ID
            bool isBranchInstr  = (idEval.opcode == OP_BRANCH);
            bool isJalrInstr    = (idEval.opcode == OP_JALR);
            bool isJalInstr     = (idEval.opcode == OP_JAL);
            bool branchUsesRegs = isBranchInstr || isJalrInstr;  // needs operands
            bool isControlFlow  = branchUsesRegs || isJalInstr;   // redirects PC

            // detect branch-dependent stalls (arith-branch and load-branch) against EX
            if (branchUsesRegs) {
                bool dependsOnEXLoad = (!oldEX.isNop && oldEX.readsMem && oldEX.rd != 0 &&
                                        ((idEval.readsRs1 && idEval.rs1 == oldEX.rd) ||
                                         (idEval.readsRs2 && idEval.rs2 == oldEX.rd)));
                bool dependsOnEXArith = (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd != 0 &&
                                         ((idEval.readsRs1 && idEval.rs1 == oldEX.rd) ||
                                          (idEval.readsRs2 && idEval.rs2 == oldEX.rd)));
                if (dependsOnEXLoad) {
                    branchDataStall = true;
                    if (loadBranchStallLeft == 0) {
                        loadBranchStallLeft = 2;  // two-cycle stall window
                        loadUseEvent = true;      // count once per hazard
                    }
                } else if (dependsOnEXArith) {
                    branchDataStall = true;  // single-cycle stall
                }
            }

            // continue stall through remaining load->branch stall cycles
            if (loadBranchStallLeft > 0) {
                branchDataStall = true;
            }

            bool idWillAdvance = !(loadUseStall || branchDataStall || loadBranchStallLeft > 0);
            if (idWillAdvance) {
                idDecodedNext = simulator->simID(oldIF);
                newID = idDecodedNext;
            } else {
                newID = oldID;
            }

            // illegal instruction detected in ID stage (current ID instruction)
            if (!idEval.isLegal) {
                idIllegalException = true;
                nextPC = idEval.nextPC;  // EXCEPTION_HANDLER
            } else if (idWillAdvance && isControlFlow && !wbMemException) {
                // Recompute control-flow decision for the instruction in ID after forwarding
                Simulator::Instruction resolved = simulator->simNextPCResolution(idEval);
                uint64_t fallThrough = idEval.PC + 4;
                branchTaken = (resolved.nextPC != fallThrough);
                nextPC = resolved.nextPC;
            }
        }

        // --------------------------------------------------------
        // IF stage: fetch next instruction (unless stalled or exception)
        // --------------------------------------------------------
        bool iMissThisCycle = false;
        if (dStallActive || loadUseStall || branchDataStall || loadBranchStallLeft > 0 || idIllegalException || wbMemException) {
            newIF = oldIF;  // frozen
        } else {
            bool iHit = iCache->access(PC, CACHE_READ);
            if (!iHit) {
                iStallLeft += iCache->config.missLatency;
                iMissThisCycle = true;
                newIF = oldIF;  // fetch stalled
            } else {
                newIF = simulator->simIF(PC);
                if (branchTaken) {
                    newIF.status = SPECULATIVE;
                }
            }
        }

        // --------------------------------------------------------
        // control hazard: mark IF SPECULATIVE on taken control transfer;
        // defer squash to next cycle so SPECULATIVE is visible in dumps
        bool scheduleSquashNextIF = false;
        if (branchTaken && !idIllegalException && !wbMemException && !loadUseStall && !branchDataStall && loadBranchStallLeft == 0) {
            newIF.status = SPECULATIVE;
            scheduleSquashNextIF = true;
        }

        // --------------------------------------------------------
        // exceptions from ID or WB squash younger instructions
        // ID-detected illegal: squash IF/ID only; allow older EX/MEM/WB to complete
        // WB-detected memory exception: squash IF/ID/EX/MEM (younger than faulting)
        if (wbMemException) {
            newIF = nop(SQUASHED);
            newID = nop(SQUASHED);
            newEX = nop(SQUASHED);
            newMEM = nop(SQUASHED);
            // newWB already holds excepting instruction
        } else if (idIllegalException) {
            newIF = nop(SQUASHED);
            newID = nop(SQUASHED);
            // EX/MEM/WB keep their older instructions
        }

        // Apply deferred IF squash for taken control transfer
        if (scheduleSquashNextIF && !idIllegalException && !wbMemException) {
            squashNextIF = true;
        }

        // --------------------------------------------------------
        // PC update:
        //  - Freeze on any stall (load-use, branch dep, i-miss, d-stall)
        //  - Otherwise: use nextPC from branch / exception logic
        // --------------------------------------------------------
        bool freezePC = loadUseStall || branchDataStall || (loadBranchStallLeft > 0) || dStallActive || dMissThisCycle || iStallActive || iMissThisCycle;

        // WB exception must take priority and force PC to handler
        if (wbMemException) {
            PC = nextPC;  // nextPC already set to EXCEPTION_HANDLER
        } else if (idIllegalException) {
            PC = nextPC;  // jump to handler even if stalls were present
        } else if (!freezePC) {
            PC = nextPC;
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
    stats.dynamicInstructions = simulator->getDin();
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
