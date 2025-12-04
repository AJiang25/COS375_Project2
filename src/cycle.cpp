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

// State to prevent double-counting I-cache hits on stall resume
static bool     wasIStalled = false;
static bool     stalledForHazard = false;

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
    iStallLeft = dStallLeft = 0;
    wasIStalled = false;
    stalledForHazard = false;

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

        // // apply any pending squash of IF from prior taken branch (for visibility)
        // if (squashNextIF) {
        //     pipelineInfo.ifInst = nop(SQUASHED);
        //     pipelineInfo.idInst = nop(SQUASHED);
        //     squashNextIF = false;
        // }

        pipeState.cycle = cycleCount;

        // snapshot current pipeline registers
        Simulator::Instruction oldIF = pipelineInfo.ifInst;
        Simulator::Instruction oldID = pipelineInfo.idInst;
        Simulator::Instruction oldEX = pipelineInfo.exInst;
        Simulator::Instruction oldMEM = pipelineInfo.memInst;
        Simulator::Instruction oldWB = pipelineInfo.wbInst;
        bool illegalInID = (!oldID.isNop && !oldID.isLegal);

        // new pipeline registers (initialized as idle NOPs)
        Simulator::Instruction newIF = nop(IDLE);
        Simulator::Instruction newID = nop(IDLE);
        Simulator::Instruction newEX = nop(IDLE);
        Simulator::Instruction newMEM = nop(IDLE);
        Simulator::Instruction newWB = nop(IDLE);

        bool branchTaken         = false;
        bool idIllegalException  = false;
        bool wbMemException      = false;

        // if prev cycle scheduled a squash of the wrong‑path IF/ID,
        // rem it here and clear the flag. then render the squash
        // in the ID stage (the wrong‑path IF from last cycle) but allow
        // the new IF to fetch from corrected PC
        bool applyDeferredSquashToID = false;
        bool deferredSquashRenderedInID = false;
        if (squashNextIF) {
            applyDeferredSquashToID = true;
            squashNextIF = false;
        }

        // default next PC is fall‑through
        uint64_t nextPC = PC + 4;

        // current stall state
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
            // Preserve status from prior stage for NOPs (idle/bubble propagation)
            if (newWB.isNop) {
                newWB.status = oldMEM.status;
            }

            // Retired dynamic-instruction counting:
            //  - Count all non-NOP, legal, non-exception instructions that were
            //    originally counted @ decode (dinCounted)
            if (!newWB.isNop && newWB.isLegal && !newWB.memException && newWB.dinCounted) {
                dynRetired++;
            }

            if (!newWB.isLegal || newWB.memException) {
                std::cerr << "[cycle " << cycleCount << "] WB exception @ PC 0x" << std::hex
                          << oldMEM.PC << std::dec
                          << " (memException=" << newWB.memException << ", legal="
                          << newWB.isLegal << ")\n";
                wbMemException = true;
                if (simulator != nullptr) {
                    std::cerr << "[DIN] disable cycle=" << cycleCount
                              << " reason=WB-exc PC=0x" << std::hex << oldMEM.PC
                              << std::dec << std::endl;
                    simulator->disableDinCounting();
                }
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
            bool exIsLoad = (!oldEX.isNop && oldEX.readsMem && !oldEX.writesMem);
            if (exIsLoad && !oldID.isNop && oldEX.rd != 0) {
                bool hazardOnRs1 = (oldID.readsRs1 && oldID.rs1 == oldEX.rd);
                // store data (rs2) forwarded WB->MEM; don't stall for that case
                bool hazardOnRs2 = (oldID.readsRs2 && oldID.rs2 == oldEX.rd && !oldID.writesMem);
                if (hazardOnRs1 || hazardOnRs2) {
                    loadUseStall = true;
                    loadUseEvent = true;
                }
            }
        }

        // Branch-Data Hazard Check (Arith-Branch & Load-Branch)
        // Check hazards for instr currently in ID (oldID)
        if (!dStallActive && !loadUseStall) {
             // branch detection uses opcode of instr in ID
            bool isBranchInstr  = (oldID.opcode == OP_BRANCH);
            bool isJalrInstr    = (oldID.opcode == OP_JALR);
            bool isJalInstr     = (oldID.opcode == OP_JAL);
            bool branchUsesRegs = isBranchInstr || isJalrInstr;  // needs operands

            if (branchUsesRegs) {
                bool dependsOnEXLoad = (!oldEX.isNop && oldEX.readsMem && oldEX.rd != 0 &&
                                        ((oldID.readsRs1 && oldID.rs1 == oldEX.rd) ||
                                         (oldID.readsRs2 && oldID.rs2 == oldEX.rd)));
                bool dependsOnEXArith = (!oldEX.isNop && oldEX.writesRd && !oldEX.readsMem && oldEX.rd != 0 &&
                                         ((oldID.readsRs1 && oldID.rs1 == oldEX.rd) ||
                                          (oldID.readsRs2 && oldID.rs2 == oldEX.rd)));
                if (dependsOnEXLoad) {
                    branchDataStall = true;
                    if (loadBranchStallLeft == 0) {
                        loadBranchStallLeft = 2;  // two-cycle stall window
                        loadUseEvent = true;      // count once per hazard
                    }
                } else if (dependsOnEXArith) {
                    // one-cycle arithmetic-branch stall
                    branchDataStall = true;
                    if (loadBranchStallLeft == 0) {
                        loadBranchStallLeft = 1;
                    }
                }
            }
        }

        // Continue stall through remaining load->branch stall cycles
        if (loadBranchStallLeft > 0) {
            branchDataStall = true;
        }

        if (loadUseStall || branchDataStall) {
            std::cerr << "[HZD] cycle " << cycleCount
                      << " loadUse=" << loadUseStall
                      << " brData=" << branchDataStall
                      << " ID.PC=0x" << std::hex << oldID.PC
                      << " EX.PC=0x" << oldEX.PC
                      << " MEM.PC=0x" << oldMEM.PC
                      << " loadBranchStallLeft=" << std::dec << loadBranchStallLeft
                      << std::endl;
        }

        bool iMissThisCycle = false;

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
                    // miss penalty: stall for full missLatency cycles AFTER this
                    // miss cycle so total visible penalty matches ref timing
                    dStallLeft += dCache->config.missLatency;
                    dMissThisCycle = true;
                    std::cerr << "[DDBG] cycle " << cycleCount
                              << " D-miss PC=0x" << std::hex << oldEX.PC
                              << " addr=0x" << memInput.memAddress
                              << " missLat=" << std::dec << dCache->config.missLatency
                              << std::endl;
                } else {
                    std::cerr << "[DDBG] cycle " << cycleCount
                              << " D-hit  PC=0x" << std::hex << oldEX.PC
                              << " addr=0x" << memInput.memAddress
                              << std::dec << std::endl;
                }
            }

            if (dMissThisCycle) {
                // keep instruction in MEM while stall counts down (with computed addr)
                newMEM = simulator->simMEM(memInput);
            } else {
                newMEM = simulator->simMEM(memInput);
            }
            // preserve status from prior stage for NOPs
            if (newMEM.isNop) {
                newMEM.status = oldEX.status;
            }
        } else {
            // EX was NOP, propagate its status to MEM
            newMEM = nop(oldEX.status);
        }

        // if MEM is stalled from prior miss, WB gets a bubble
        if (dStallActive) {
            newWB = nop(BUBBLE);
        }

        bool dStallNow = dStallActive || dMissThisCycle;

        // --------------------------------------------------------
        // IF stage (prefetch for this cycle) to know I-miss before ID gating
        // --------------------------------------------------------
        bool iStallActiveNow = (iStallLeft > 0);
        bool wasIStalledBefore = wasIStalled;  // capture before IF logic clears it
        Simulator::Instruction fetchedIF = oldIF;
        
        // if pipeline frozen (hazards/D-stall), keep oldIF but retag its PC w/
        // current global PC (matches correct behavior from illegal
        // and ensures IF shows the PC that would've been fetched)
        if (dStallNow || loadUseStall || branchDataStall || loadBranchStallLeft > 0 ||
            idIllegalException || wbMemException) {
            fetchedIF = oldIF;
            fetchedIF.PC = PC;
            // during branch data stall, IF is speculative (waiting on branch resolution)
            if (branchDataStall || loadBranchStallLeft > 0) {
                fetchedIF.status = SPECULATIVE;
            } else {
                fetchedIF.status = NORMAL;
            }
            nextPC = PC;        // don't adv PC when stalled
            stalledForHazard = true; // flag we hold oldIF due to hazard
        } else if (iStallActiveNow) {
            // I‑cache miss penalty in progress: keep showing the same PC in IF
            fetchedIF = oldIF;
            fetchedIF.PC = PC;
            fetchedIF.status = BUBBLE;
            wasIStalled = true;
            stalledForHazard = false;
        } else {
            if (wasIStalled || stalledForHazard) {
                // resume after stall OR hazard: fetch WITHOUT re‑accessing cache
                fetchedIF = simulator->simIF(PC);
                wasIStalled = false;
                stalledForHazard = false;
            } else {
                // normal operation: probe I‑cache for this PC
                bool iHit = iCache->access(PC, CACHE_READ);
                if (!iHit) {
                    // miss penalty: stall for missLatency cycles total (in addition to this miss cycle)
                    // count down iStallLeft @ start of each cycle, so setting it to missLatency
                    // here gives exactly missLatency stall cycles *AFTERRR* this miss cycle 
                    // => matching reference timing
                    iStallLeft += iCache->config.missLatency;
                    iMissThisCycle = true;
                    // keep showing same PC in IF; ID will receive a bubble
                    fetchedIF = oldIF;
                    fetchedIF.PC = PC;
                    if (oldIF.status == IDLE) {
                        fetchedIF.status = IDLE;
                    } else {
                        fetchedIF.status = BUBBLE;
                    }
                    wasIStalled = true;
                } else {
                    fetchedIF = simulator->simIF(PC);
                }
            }
        }

        // from ID's view, stall during the active miss window + the cycle
        // immediately following completion (tracked w/ wasIStalledBefore)
        // the first miss cycle should only block decode if IF doesn't yet hold
        // a real instruction (pipeline still idle)
        bool oldIFIsIdle = (oldIF.isNop && oldIF.status == IDLE);
        bool iStallForID = iStallActiveNow || wasIStalledBefore || (iMissThisCycle && oldIFIsIdle);

        // --------------------------------------------------------
        // EX stage: ID -> EX with forwarding (unless stalling)
        // --------------------------------------------------------
        if (dStallActive) {
            // If D-stall continues, hold existing EX (bubble)
            newEX = oldEX; 
        } else if (illegalInID) {
            newEX = nop(SQUASHED);  // illegal squashed before EX
        } else if (!loadUseStall && !branchDataStall && loadBranchStallLeft == 0) {
            Simulator::Instruction exInput = oldID;

            // EX -> EX fwd (from instr just finished EX, moving to MEM)
            if (!oldEX.isNop && oldEX.writesRd && oldEX.rd != 0) {
                uint64_t fwdVal = oldEX.arithResult; 
                if (exInput.readsRs1 && exInput.rs1 == oldEX.rd) {
                    exInput.op1Val = fwdVal;
                }
                if (exInput.readsRs2 && exInput.rs2 == oldEX.rd) {
                    exInput.op2Val = fwdVal;
                }
            }

            // MEM -> EX forwarding (from instruction that just finished MEM, moving to WB)
            // Only if not forwarded from EX (priority to youngest)
            if (!oldMEM.isNop && oldMEM.writesRd && oldMEM.rd != 0) {
                uint64_t fwdVal = oldMEM.readsMem ? oldMEM.memResult : oldMEM.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldMEM.rd &&
                    !(oldEX.writesRd && oldEX.rd == oldMEM.rd && oldEX.rd != 0)) {
                    exInput.op1Val = fwdVal;
                }
                if (exInput.readsRs2 && exInput.rs2 == oldMEM.rd &&
                    !(oldEX.writesRd && oldEX.rd == oldMEM.rd && oldEX.rd != 0)) {
                    exInput.op2Val = fwdVal;
                }
            }

            // WB -> EX forwarding (only if not alr forwarded from MEM or EX)
            if (!oldWB.isNop && oldWB.writesRd && oldWB.rd != 0) {
                uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                if (exInput.readsRs1 && exInput.rs1 == oldWB.rd &&
                    !(oldMEM.writesRd && oldMEM.rd == oldWB.rd && oldMEM.rd != 0) &&
                    !(oldEX.writesRd && oldEX.rd == oldWB.rd && oldEX.rd != 0)) {
                    exInput.op1Val = fwdVal;
                }
                if (exInput.readsRs2 && exInput.rs2 == oldWB.rd &&
                    !(oldMEM.writesRd && oldMEM.rd == oldWB.rd && oldMEM.rd != 0) &&
                    !(oldEX.writesRd && oldEX.rd == oldWB.rd && oldEX.rd != 0)) {
                    exInput.op2Val = fwdVal;
                }
            }

            newEX = simulator->simEX(exInput);
            // preserve status from prior stage for NOPs
            if (newEX.isNop) {
                newEX.status = oldID.status;
            }
        } else {
            // bubble btwn ID and EX when stalling on load-use/branch dep
            newEX = nop(BUBBLE);
        }

        // --------------------------------------------------------
        // ID stage: IF -> ID (unless stalled)
        // Branch hazards/resolution operate on the instruction *currently* in ID (oldID)
        // --------------------------------------------------------
        Simulator::Instruction idDecodedNext = nop(BUBBLE);  // will hold decode of oldIF if we advance
        Simulator::Instruction idEval = oldID;                // instruction presently in ID

        if (dStallActive) {
            // D-cache stall in progress: younger stages see a bubble until MEM completes
            newID = nop(BUBBLE);
        } else if (applyDeferredSquashToID) {
            // wrong-path IF from prior cycle: render as squashed and skip decode
            newID = nop(SQUASHED);
            deferredSquashRenderedInID = true;
        } else {
            // forward operands for instruction currently in ID (idEval)
            if (idEval.readsRs1 && idEval.rs1 != 0) {
                // newMEM (was oldEX) -> ID
                if (!newMEM.isNop && newMEM.writesRd && newMEM.rd == idEval.rs1) {
                    uint64_t fwdVal = newMEM.readsMem ? newMEM.memResult : newMEM.arithResult;
                    idEval.op1Val = fwdVal;
                // newWB (was oldMEM) -> ID
                } else if (!newWB.isNop && newWB.writesRd && newWB.rd == idEval.rs1) {
                    uint64_t fwdVal = newWB.readsMem ? newWB.memResult : newWB.arithResult;
                    idEval.op1Val = fwdVal;
                // oldWB (was oldWB) -> ID
                } else if (!oldWB.isNop && oldWB.writesRd && oldWB.rd == idEval.rs1) {
                    uint64_t fwdVal = oldWB.readsMem ? oldWB.memResult : oldWB.arithResult;
                    idEval.op1Val = fwdVal;
                }
            }
            if (idEval.readsRs2 && idEval.rs2 != 0) {
                if (!newMEM.isNop && newMEM.writesRd && newMEM.rd == idEval.rs2) {
                    uint64_t fwdVal = newMEM.readsMem ? newMEM.memResult : newMEM.arithResult;
                    idEval.op2Val = fwdVal;
                } else if (!newWB.isNop && newWB.writesRd && newWB.rd == idEval.rs2) {
                    uint64_t fwdVal = newWB.readsMem ? newWB.memResult : newWB.arithResult;
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

            bool idWillAdvance = !(loadUseStall || branchDataStall || loadBranchStallLeft > 0 || dStallActive);

            // illegal instruction detected in ID stage for the instruction
            // currently resident in ID (idEval); handles cases where an
            // illegal instruction was already in ID and we not advancing
            // due bc stall
            if (!idEval.isLegal) {
                std::cerr << "[cycle " << cycleCount << "] ID illegal (resident) PC 0x" << std::hex
                          << idEval.PC << std::dec << "\n";
                idIllegalException = true;
                if (simulator != nullptr) {
                    std::cerr << "[DIN] disable cycle=" << cycleCount
                              << " reason=ID-illegal-res PC=0x" << std::hex << idEval.PC
                              << std::dec << std::endl;
                    simulator->disableDinCounting();
                }
                nextPC = idEval.nextPC;  // EXCEPTION_HANDLER
            } else if (idWillAdvance && isControlFlow && !wbMemException) {
                // Recompute control-flow decision for the instruction in ID after forwarding
                Simulator::Instruction resolved = simulator->simNextPCResolution(idEval);
                uint64_t fallThrough = idEval.PC + 4;
                branchTaken = (resolved.nextPC != fallThrough);
                if (branchTaken) {
                    std::cerr << "[BR] cycle " << cycleCount
                              << " ID.PC=0x" << std::hex << idEval.PC
                              << " nextPC=0x" << resolved.nextPC
                              << " fall=0x" << fallThrough
                              << std::dec << std::endl;
                }
                nextPC = resolved.nextPC;
            }

            // Decide what enters ID from IF this cycle
            if (idWillAdvance) {
                if (branchTaken && !idIllegalException && !wbMemException) {
                    // wrong‑path instruction in IF; squash before decode so din is not counted
                    newID = nop(SQUASHED);
                    
                    // flush I-Cache stall state
                    iStallLeft = 0;
                    wasIStalled = false;
                    stalledForHazard = false; // Hazard state doesn't apply if we branch
                    
                } else if (iStallForID) {
                    // I-cache stall: insert bubble in ID, don't decode
                    // on first miss cycle, keep ID as IDLE NOP so initial
                    // pipeline state (cycle 0) matches ref; on subsequent stall
                    // cycles, insert BUBBLEs
                    if (iMissThisCycle && oldIF.status == IDLE) {
                        newID = nop(IDLE);
                    } else {
                        newID = nop(BUBBLE);
                    }
                } else {
                    // Normal decode of oldIF
                    idDecodedNext = simulator->simID(oldIF);

                    // if *incoming* instruction (from IF) is illegal, raise ID-stage
                    // illegal exception immediately so redirect to handler happens
                    // in the same cycle this instruction first appears in ID
                    if (!idDecodedNext.isLegal) {
                        std::cerr << "[cycle " << cycleCount
                                  << "] ID illegal (new decode) PC 0x" << std::hex
                                  << oldIF.PC << std::dec << "\n";
                        // count this illegal instruction as dynamic instruction
                        // exactly once, using dinCounted tag from decode
                        if (idDecodedNext.dinCounted) {
                            dynRetired++;
                        }
                        idIllegalException = true;
                        if (simulator != nullptr) {
                            std::cerr << "[DIN] disable cycle=" << cycleCount
                                      << " reason=ID-illegal-new PC=0x" << std::hex << oldIF.PC
                                      << std::dec << std::endl;
                            simulator->disableDinCounting();
                        }
                        nextPC = idDecodedNext.nextPC;  // EXCEPTION_HANDLER
                    }

                    newID = idDecodedNext;
                    // preserve status from prior stage for NOPs (idle/bubble propagation)
                    if (newID.isNop) {
                        newID.status = oldIF.status;
                    }
                }
            } else {
                newID = oldID;
            }

            // DEBUG: track when ID actually performs new decode vs holding/bubbling
            // scoped to fib run for log focus
            if (output.find("fib") != std::string::npos) {
                std::cerr << "[IDDBG] cycle " << cycleCount
                          << " idWillAdvance=" << idWillAdvance
                          << " iStallForID=" << iStallForID
                          << " loadUseStall=" << loadUseStall
                          << " branchDataStall=" << branchDataStall
                          << " dStallActive=" << dStallActive
                          << " oldIF.PC=0x" << std::hex << oldIF.PC
                          << " oldID.PC=0x" << oldID.PC
                          << " newID.PC=0x" << newID.PC
                          << " newID.status=" << newID.status
                          << std::dec << std::endl;
            }
        }

        // if a squash was deferred from previous cycle (branch/exception),
        // render it in ID now (wrong‑path IF from last cycle)
        if (applyDeferredSquashToID && !deferredSquashRenderedInID) {
            newID = nop(SQUASHED);
        }

        // --------------------------------------------------------
        // IF stage: fetch next instruction (unless stalled or exception)
        // --------------------------------------------------------
        newIF = fetchedIF;
        if (branchTaken && !idIllegalException && !wbMemException && !dStallNow && !iStallActiveNow && !iMissThisCycle && !loadUseStall && !branchDataStall && loadBranchStallLeft == 0) {
            newIF.status = SPECULATIVE;
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
            // squash younger instructions (ID/EX/MEM) immediately;
            // IF shows handler PC with NORMAL status (no annotation)
            newIF = simulator->simIF(nextPC);  // fetch from handler
            newIF.PC = nextPC;
            newIF.status = NORMAL;
            newID = nop(SQUASHED);
            newEX = nop(SQUASHED);
            newMEM = nop(SQUASHED);
            // clear any IF stall state since we redirect fetch
            wasIStalled = false;
            stalledForHazard = false;
            iStallLeft = 0;
        } else if (idIllegalException) {
            // ID-detected illegal: squash IF/ID only; allow older EX/MEM/WB to complete
            // dont fetch from handler in this cycle; redirect to handler PC via PC update logic
            // so illegal instruction is still visible in ID for this cycle; wrong-path
            // IF/ID will be rendered as squashed in next cycle via squashNextIF / applyDeferredSquashToID
            newIF = fetchedIF;
            newIF.status = NORMAL;
            // clear any IF stall state since we redirect fetch
            wasIStalled = false;
            stalledForHazard = false;
            iStallLeft = 0;

            // request deferred squash of IF/ID in next cycle (for display),
            // similar to how control hazards are rendered
            squashNextIF = true;
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
        bool freezePC = loadUseStall || branchDataStall || (loadBranchStallLeft > 0) || dStallNow || (iStallLeft > 0) || iMissThisCycle;

        // DEBUG: early setup and final loop/exception region
        if ((cycleCount >= 15 && cycleCount <= 25) ||
            (cycleCount >= 140 && cycleCount <= 160)) {
            std::cerr << "[DBG] cycle=" << cycleCount
                      << " PC=0x" << std::hex << PC << std::dec
                      << " nextPC=0x" << std::hex << nextPC << std::dec
                      << " freezePC=" << freezePC
                      << " dStallActive=" << dStallActive
                      << " dMissThis=" << dMissThisCycle
                      << " dStallLeft=" << dStallLeft
                      << " iStallLeft=" << iStallLeft
                      << " iMissThis=" << iMissThisCycle
                      << " loadUseStall=" << loadUseStall
                      << " branchDataStall=" << branchDataStall
                      << " loadBranchStallLeft=" << loadBranchStallLeft
                      << " wbMemException=" << wbMemException
                      << " idIllegalException=" << idIllegalException
                      << " oldIF.PC=0x" << std::hex << oldIF.PC
                      << " newIF.PC=0x" << newIF.PC << std::dec
                      << std::endl;
        }

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

    SimulationStats stats{};
    // use retired dynamic-instruction count from pipeline behavior,
    // instead of raw decode count, so squashed or re-decoded
    // instructions aren't double-counted
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
