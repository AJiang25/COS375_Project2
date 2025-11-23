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
static uint64_t loadUseStallCount = 0;
static uint64_t loadBranchStallLeft = 0;  // tracks two-cycle load->branch stall window

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

            if (oldEX.readsMem || oldEX.writesMem) {
                bool dHit = dCache->access(oldEX.memAddress,
                                           oldEX.writesMem ? CACHE_WRITE : CACHE_READ);
                if (!dHit) {
                    dStallLeft += dCache->config.missLatency;
                    dMissThisCycle = true;
                }
            }

            if (dMissThisCycle) {
                // keep instruction in MEM while stall counts down
                newMEM = oldEX;
            } else {
                newMEM = simulator->simMEM(oldEX);
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
        // also resolves branches in ID
        // --------------------------------------------------------
        Simulator::Instruction idOut = nop(BUBBLE);
        if (dStallActive) {
            newID = oldID;  // freeze during D stall
        } else {
            idOut = simulator->simID(oldIF);

            // branch operand forwarding (ID uses results from older stages)
            if (idOut.readsRs1 && idOut.rs1 != 0) {
                // prefer EX (ALU result available), then MEM, then WB
                if (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd == idOut.rs1) {
                    idOut.op1Val = oldEX.arithResult;
                } else if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd == idOut.rs1) {
                    uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                    idOut.op1Val = fwdVal;
                } else if (!oldWB.isNop && oldWB.writesRd && oldWB.rd == idOut.rs1) {
                    uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                    idOut.op1Val = fwdVal;
                }
            }
            if (idOut.readsRs2 && idOut.rs2 != 0) {
                if (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd == idOut.rs2) {
                    idOut.op2Val = oldEX.arithResult;
                } else if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd == idOut.rs2) {
                    uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                    idOut.op2Val = fwdVal;
                } else if (!oldWB.isNop && oldWB.writesRd && oldWB.rd == idOut.rs2) {
                    uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                    idOut.op2Val = fwdVal;
                }
            }

            // Branch detection uses opcode, not predicted target
            bool isBranchInstr = (idOut.opcode == OP_BRANCH);
            bool isJalrInstr   = (idOut.opcode == OP_JALR);
            bool branchLike    = isBranchInstr || isJalrInstr;

            // detect branch-dependent stalls (arith-branch and load-branch)
            if (branchLike) {
                bool dependsOnEXLoad = (!oldEX.isNop && oldEX.readsMem && oldEX.rd != 0 &&
                                        ((idOut.readsRs1 && idOut.rs1 == oldEX.rd) ||
                                        (idOut.readsRs2 && idOut.rs2 == oldEX.rd)));
                bool dependsOnEXArith = (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd != 0 &&
                                        ((idOut.readsRs1 && idOut.rs1 == oldEX.rd) ||
                                        (idOut.readsRs2 && idOut.rs2 == oldEX.rd)));
                if (dependsOnEXLoad) {
                    branchDataStall = true;
                    if (loadBranchStallLeft == 0) {
                        loadBranchStallLeft = 2;  // two-cycle stall window
                        loadUseEvent = true;      // count once per hazard
                    }
                } else if (dependsOnEXArith) {
                    branchDataStall = true;  // single-cycle stall via branchDataStall
                }
            }

            // continue stall through remaining load->branch stall cycles
            if (loadBranchStallLeft > 0) {
                branchDataStall = true;
            }

            if (loadUseStall || branchDataStall || loadBranchStallLeft > 0) {
                newID = oldID;  // hold ID
            } else {
                newID = idOut;

                // illegal instruction detected in ID
                if (!idOut.isLegal) {
                    idIllegalException = true;
                    nextPC = idOut.nextPC;  // EXCEPTION_HANDLER
                } else if (branchLike && !wbMemException) {
                    // Recompute branch decision after forwarding using helper
                    Simulator::Instruction resolved = simulator->simNextPCResolution(idOut);
                    uint64_t fallThrough = idOut.PC + 4;
                    branchTaken = (resolved.nextPC != fallThrough) || isJalrInstr;
                    nextPC = resolved.nextPC;
                }
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
        // control hazard: squash the speculatively fetched IF instruction
        // when a branch is taken in ID
        // --------------------------------------------------------
        // if branch taken in ID, mark fetched instruction SPECULATIVE and squash on next line so SPECULATIVE visible in dumps
        bool squashIFnext = false;
        if (branchTaken && !idIllegalException && !wbMemException && !loadUseStall && !branchDataStall && loadBranchStallLeft == 0) {
            newIF.status = SPECULATIVE;
            squashIFnext = true;
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

        // Apply squash of speculative IF after visibility
        if (squashIFnext && !idIllegalException && !wbMemException) {
            newIF = nop(SQUASHED);
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
