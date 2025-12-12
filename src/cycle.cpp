#include "cycle.h"

#include <climits>
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

static uint64_t PC = 0;
// Cache miss handling
static uint64_t iMissRemaining = 0;
static bool iMissActive = false;  // True when waiting for I-cache miss to resolve
static uint64_t dMissRemaining = 0;
static bool dMissActive = false;
static bool dMissIsLoad = false;  // True if D-cache miss is for a load (vs store)
static Simulator::Instruction latchedMemInst;
// Exception/flush handling - pending from previous cycle
static bool pendingFlush = false;
static uint64_t pendingFlushPC = 0;
// Stats
static uint64_t loadUseStallCount = 0;
static uint64_t committedDin = 0;
static uint64_t lastCommittedPC = UINT64_MAX;

Simulator::Instruction doneInst;

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction n;
    n.instruction = 0x00000013;
    n.isLegal = true;
    n.isNop = true;
    n.status = status;
    return n;
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
    doneInst = nop(IDLE);
    cycleCount = 0;
    PC = 0;
    iMissRemaining = dMissRemaining = 0;
    iMissActive = dMissActive = dMissIsLoad = false;
    pendingFlush = false;
    pendingFlushPC = 0;
    loadUseStallCount = committedDin = 0;
    lastCommittedPC = UINT64_MAX;
    pipelineInfo = {};
    pipelineInfo.ifInst = nop(IDLE);
    pipelineInfo.idInst = nop(IDLE);
    pipelineInfo.exInst = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst = nop(IDLE);
    return SUCCESS;
}

bool hazard(const Simulator::Instruction& dstInst, uint64_t srcReg) {
    return dstInst.writesRd && dstInst.rd != 0 && dstInst.rd == srcReg;
}

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed
Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    auto status = SUCCESS;
    PipeState pipeState = {0};

    while (cycles == 0 || count < cycles) {
        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        // Check if there's a pending flush from previous cycle
        // This handles exception redirection and squashing
        bool applyFlush = pendingFlush;
        uint64_t flushPC = pendingFlushPC;
        pendingFlush = false;
        
        if (applyFlush) {
            // Squash instructions in IF and ID (they were younger than the excepting instruction)
            // The excepting instruction itself was already shown in ID last cycle
            PC = flushPC;
            iMissRemaining = 0;
        }

        Simulator::Instruction ID = pipelineInfo.idInst;
        Simulator::Instruction EX = pipelineInfo.exInst;

        bool stall = false;
        bool flush = false;  // Branch flush (immediate effect)
        bool branchStall = false;
        // memStall is true if D-cache miss is active from a previous cycle
        // newMissDetected is set during MEM stage if a new miss is detected this cycle
        bool memStall = dMissActive;
        bool newMissDetected = false;

        // Check load-use stalls (no need to stall if the destination register is x0)
        bool loadUseStallTriggered = false;
        if (EX.readsMem && EX.rd != 0 && !EX.isNop) {
            bool hazardRs1 = (EX.rd == ID.rs1 && ID.readsRs1);
            bool hazardRs2 = (EX.rd == ID.rs2 && ID.readsRs2);
            if (hazardRs1 || hazardRs2) {
                // Special-case: load -> store data (rs2) should NOT stall; forward WB->MEM
                bool isStore = ID.writesMem;
                bool isOnlyStoreDataHazard = (!hazardRs1) && hazardRs2 && isStore;
                if (!isOnlyStoreDataHazard) {
                    stall = true;
                    loadUseStallTriggered = true;
                }
            }
        }

        // Check arithmetic-branch stall
        if (EX.writesRd && (ID.opcode == OP_BRANCH || ID.opcode == OP_JALR) && EX.rd != 0 && !EX.isNop) {
            if ((ID.rs1 == EX.rd && ID.readsRs1) || (ID.rs2 == EX.rd && ID.readsRs2)) {
                stall = true;
            }
        }

        // load-branch stall if branch depends on MEM-stage load
        if (pipelineInfo.memInst.readsMem && pipelineInfo.memInst.writesRd &&
            (ID.opcode == OP_BRANCH || ID.opcode == OP_JALR) && pipelineInfo.memInst.rd != 0 && !pipelineInfo.memInst.isNop) {
            if ((pipelineInfo.memInst.rd == ID.rs1 && ID.readsRs1) || (pipelineInfo.memInst.rd == ID.rs2 && ID.readsRs2)) {
                stall = true;
            }
        }

        // ==================== WB STAGE ====================
        // During D-cache miss, WB gets bubbles (except first cycle when previous MEM flows through)
        Simulator::Instruction prevMEM = pipelineInfo.memInst;

        if (cycleCount >= 17 && cycleCount <= 22) {
            std::cerr << "DEBUG WB cycle " << cycleCount << ": dMissActive=" << dMissActive << std::endl;
        }
        if (dMissActive) {
            // D-cache miss in progress - WB gets a bubble
            pipelineInfo.wbInst = nop(BUBBLE);
        } else {
            pipelineInfo.wbInst = simulator->simWB(prevMEM);

            // Determine WB status based on what came from MEM
            if (prevMEM.isNop && prevMEM.status == IDLE) {
                pipelineInfo.wbInst = nop(IDLE);
            } else if (prevMEM.isNop && prevMEM.status == SQUASHED) {
                pipelineInfo.wbInst = nop(SQUASHED);
            } else if (pipelineInfo.wbInst.isNop) {
                pipelineInfo.wbInst.status = BUBBLE;
            } else {
                pipelineInfo.wbInst.status = NORMAL;
            }
        }

        // Count committed instructions (includes HALT)
        // Count every instruction that commits in WB, including repeated executions in loops
        if (!pipelineInfo.wbInst.isNop && pipelineInfo.wbInst.isLegal) {
            committedDin++;
        }
        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
        }

        // ==================== MEM STAGE with D-cache timing ====================
        Simulator::Instruction prevEX = pipelineInfo.exInst;
        bool dMissResolvingThisCycle = false;  // Track if miss resolves this cycle

        if (dMissActive) {
            // Don't modify memStall here - it's already set correctly at cycle start
            if (cycleCount >= 10 && cycleCount <= 22) {
                std::cerr << "DEBUG MEM cycle " << cycleCount << ": dMissRemaining before=" << dMissRemaining;
            }
            if (dMissRemaining > 0) {
                dMissRemaining--;
            }
            if (cycleCount >= 10 && cycleCount <= 22) {
                std::cerr << " after=" << dMissRemaining << std::endl;
            }
            if (dMissRemaining == 0) {
                // Miss resolved - execute the MEM operation
                pipelineInfo.memInst = simulator->simMEM(latchedMemInst);
                pipelineInfo.memInst.status = pipelineInfo.memInst.isNop ? BUBBLE : NORMAL;
                dMissActive = false;
                dMissResolvingThisCycle = true;  // Mark that we resolved this cycle
                // memStall remains true for this cycle; will be false next cycle
            } else {
                // Still waiting - show the latched instruction in MEM
                pipelineInfo.memInst = latchedMemInst;
                pipelineInfo.memInst.status = NORMAL;
            }
        } else {
            bool willAccessDCache = prevEX.readsMem || prevEX.writesMem;
            if (willAccessDCache && !prevEX.isNop) {
                CacheOperation op = prevEX.readsMem ? CACHE_READ : CACHE_WRITE;
                bool dHit = dCache->access(prevEX.memAddress, op);
                if (!dHit) {
                    latchedMemInst = prevEX;
                    dMissRemaining = dCache->config.missLatency;
                    dMissActive = true;
                    dMissIsLoad = prevEX.readsMem;  // Track if this is a load miss
                    pipelineInfo.memInst = latchedMemInst;
                    pipelineInfo.memInst.status = NORMAL;
                    // Mark that a new miss was detected this cycle
                    // On detection cycle: EX advances, ID gets bubble, IF stalls
                    newMissDetected = true;
                    if (cycleCount >= 10 && cycleCount <= 15) {
                        std::cerr << "DEBUG: D-cache miss detected at cycle " << cycleCount
                                  << " dMissRemaining=" << dMissRemaining << std::endl;
                    }
                } else {
                    pipelineInfo.memInst = simulator->simMEM(prevEX);
                    if (prevEX.isNop && prevEX.status == IDLE) {
                        pipelineInfo.memInst = nop(IDLE);
                    } else if (prevEX.isNop && prevEX.status == SQUASHED) {
                        pipelineInfo.memInst = nop(SQUASHED);
                    } else if (pipelineInfo.memInst.isNop) {
                        pipelineInfo.memInst.status = BUBBLE;
                    } else {
                        pipelineInfo.memInst.status = NORMAL;
                    }
                }
            } else {
                pipelineInfo.memInst = simulator->simMEM(prevEX);
                if (prevEX.isNop && prevEX.status == IDLE) {
                    pipelineInfo.memInst = nop(IDLE);
                } else if (prevEX.isNop && prevEX.status == SQUASHED) {
                    pipelineInfo.memInst = nop(SQUASHED);
                } else if (pipelineInfo.memInst.isNop) {
                    pipelineInfo.memInst.status = BUBBLE;
                } else {
                    // Both regular instructions and HALT get NORMAL status
                    pipelineInfo.memInst.status = NORMAL;
                }
            }
        }

        // Update memStall based on post-MEM state.
        // A miss detected this cycle should not stall younger stages until next cycle.
        // When a miss resolves this cycle, we still need to stall (pipeline shows stalled state
        // on the resolution cycle; normal flow resumes next cycle).
        memStall = (dMissActive && !newMissDetected) || dMissResolvingThisCycle;

        // Debug
        if (cycleCount >= 17 && cycleCount <= 22) {
            std::cerr << "DEBUG cycle " << cycleCount << ": memStall=" << memStall
                      << " dMissActive=" << dMissActive << " newMissDetected=" << newMissDetected
                      << " dMissResolvingThisCycle=" << dMissResolvingThisCycle
                      << " stall=" << stall << " branchStall=" << branchStall << std::endl;
        }

        // ==================== EX STAGE ====================
        Simulator::Instruction prevID = pipelineInfo.idInst;
        
        // If flush is being applied this cycle, the instruction in ID was the illegal/excepting one
        // It should be squashed before reaching EX
        if (applyFlush) {
            pipelineInfo.exInst = nop(SQUASHED);
        } else if (memStall) {
            // D-cache miss stall: EX holds its current instruction (don't advance)
            // pipelineInfo.exInst stays the same
        } else if (stall) {
            if (loadUseStallTriggered) {
                loadUseStallCount++;
            }
            if (prevID.isNop && prevID.status == IDLE) {
                pipelineInfo.exInst = nop(IDLE);
            } else {
                pipelineInfo.exInst = nop(BUBBLE);
            }
        } else {
            // Forwarding to EX/ID values
            if (hazard(pipelineInfo.memInst, prevID.rs1)) {
                prevID.op1Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                              : pipelineInfo.memInst.arithResult;
            } else if (hazard(pipelineInfo.wbInst, prevID.rs1)) {
                prevID.op1Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                            : pipelineInfo.wbInst.arithResult;
            } else if (hazard(doneInst, prevID.rs1)) {
                prevID.op1Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
            }
            if (hazard(pipelineInfo.memInst, prevID.rs2)) {
                prevID.op2Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                              : pipelineInfo.memInst.arithResult;
            } else if (hazard(pipelineInfo.wbInst, prevID.rs2)) {
                prevID.op2Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                            : pipelineInfo.wbInst.arithResult;
            } else if (hazard(doneInst, prevID.rs2)) {
                prevID.op2Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
            }

            if (prevID.isNop) {
                if (prevID.status == IDLE) {
                    pipelineInfo.exInst = nop(IDLE);
                } else if (prevID.status == SQUASHED) {
                    pipelineInfo.exInst = nop(SQUASHED);
                } else {
                    pipelineInfo.exInst = nop(BUBBLE);
                }
            } else {
                pipelineInfo.exInst = simulator->simEX(prevID);
                if (pipelineInfo.exInst.isNop) {
                    pipelineInfo.exInst.status = BUBBLE;
                } else {
                    // Both regular instructions and HALT get NORMAL status
                    pipelineInfo.exInst.status = NORMAL;
                }
            }
        }

        // ==================== ID STAGE ====================
        Simulator::Instruction prevIF = pipelineInfo.ifInst;
        
        // If flush is being applied this cycle, squash the instruction that was in IF
        if (applyFlush) {
            pipelineInfo.idInst = nop(SQUASHED);
        } else if (!(stall || memStall)) {
            if (prevIF.isNop) {
                // IF produced a NOP (during cache miss or after flush)
                if (pipelineInfo.idInst.isNop && pipelineInfo.idInst.status == IDLE) {
                    if (prevIF.status != IDLE) {
                        pipelineInfo.idInst = nop(BUBBLE);
                    }
                } else {
                    if (prevIF.status == SQUASHED) {
                        pipelineInfo.idInst = nop(SQUASHED);
                    } else {
                        pipelineInfo.idInst = nop(BUBBLE);
                    }
                }
            } else {
                Simulator::Instruction newIDInst = simulator->simID(prevIF);

                // Illegal instruction detected in ID -> schedule exception for next cycle
                if (!newIDInst.isNop && !newIDInst.isLegal) {
                    pendingFlush = true;
                    pendingFlushPC = 0x8000;
                    // ID shows the illegal instruction with NORMAL status (displays as "ILLEGAL")
                    pipelineInfo.idInst = newIDInst;
                    pipelineInfo.idInst.status = NORMAL;
                } else {
                    // Branch/JALR forwarding hazards
                    if (newIDInst.opcode == OP_BRANCH || newIDInst.opcode == OP_JALR) {
                        if (hazard(pipelineInfo.exInst, newIDInst.rs1) || hazard(pipelineInfo.exInst, newIDInst.rs2)) {
                            branchStall = true;
                        }
                        if ((hazard(pipelineInfo.memInst, newIDInst.rs1) ||
                             hazard(pipelineInfo.memInst, newIDInst.rs2)) && pipelineInfo.memInst.readsMem) {
                            branchStall = true;
                        }
                    }

                    if (branchStall) {
                        int loadBranchHazardCount = 0;
                        if (newIDInst.opcode == OP_BRANCH || newIDInst.opcode == OP_JALR) {
                            if (EX.readsMem && !EX.isNop && (hazard(EX, newIDInst.rs1) || hazard(EX, newIDInst.rs2))) {
                                loadBranchHazardCount++;
                            }
                            if (pipelineInfo.memInst.readsMem && !pipelineInfo.memInst.isNop &&
                                (hazard(pipelineInfo.memInst, newIDInst.rs1) ||
                                 hazard(pipelineInfo.memInst, newIDInst.rs2))) {
                                loadBranchHazardCount++;
                            }
                        }
                        loadUseStallCount += loadBranchHazardCount;
                        pipelineInfo.idInst = nop(BUBBLE);
                    } else {
                        // Forward to branch operands if ready
                        if (newIDInst.opcode == OP_BRANCH || newIDInst.opcode == OP_JALR) {
                            if (hazard(pipelineInfo.memInst, newIDInst.rs1)) {
                                newIDInst.op1Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                                                   : pipelineInfo.memInst.arithResult;
                            } else if (hazard(pipelineInfo.wbInst, newIDInst.rs1)) {
                                newIDInst.op1Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                                                : pipelineInfo.wbInst.arithResult;
                            } else if (hazard(doneInst, newIDInst.rs1)) {
                                newIDInst.op1Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
                            }

                            if (hazard(pipelineInfo.memInst, newIDInst.rs2)) {
                                newIDInst.op2Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                                                   : pipelineInfo.memInst.arithResult;
                            } else if (hazard(pipelineInfo.wbInst, newIDInst.rs2)) {
                                newIDInst.op2Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                                                : pipelineInfo.wbInst.arithResult;
                            } else if (hazard(doneInst, newIDInst.rs2)) {
                                newIDInst.op2Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
                            }
                            newIDInst = simulator->simNextPCResolution(newIDInst);
                        }

                        pipelineInfo.idInst = newIDInst;
                        if (pipelineInfo.idInst.isNop) {
                            pipelineInfo.idInst.status = BUBBLE;
                        } else {
                            // Both regular instructions and HALT get NORMAL status
                            pipelineInfo.idInst.status = NORMAL;
                        }

                        // Branch resolved: flush if target != PC+4 (immediate effect)
                        // Don't check for HALT since it doesn't have nextPC set
                        if (!pipelineInfo.idInst.isHalt && pipelineInfo.idInst.nextPC != prevIF.PC + 4) {
                            flush = true;
                            PC = pipelineInfo.idInst.nextPC;
                        }
                    }
                }
            }
        }

        // ==================== IF STAGE ====================
        if (cycleCount >= 19 && cycleCount <= 22) {
            std::cerr << "DEBUG IF cycle " << cycleCount << ": stall=" << stall << " branchStall=" << branchStall
                      << " memStall=" << memStall << " newMissDetected=" << newMissDetected
                      << " flush=" << flush << " applyFlush=" << applyFlush << std::endl;
        }
        if (applyFlush) {
            // Exception redirect: start fetching from exception handler
            // Do cache access so miss penalty starts this cycle
            bool iHit = iCache->access(PC, CACHE_READ);
            if (!iHit) {
                // Miss - set full penalty, will fetch when it reaches 0
                iMissRemaining = iCache->config.missLatency;
                iMissActive = true;
                pipelineInfo.ifInst = nop(NORMAL);
                pipelineInfo.ifInst.PC = PC;
            } else {
                // Hit - fetch immediately
                pipelineInfo.ifInst = simulator->simIF(PC);
                pipelineInfo.ifInst.status = NORMAL;
                PC = PC + 4;
                iMissActive = false;
            }
        } else if (stall || branchStall || memStall || newMissDetected) {
            // Hold IF - but continue I-cache miss countdown during D-cache stall
            if (iMissRemaining > 0) {
                iMissRemaining--;
                if (iMissRemaining == 0) {
                    // Miss resolved; allow fetch when stall clears
                    iMissActive = false;
                }
            }
            // If no active I-miss, keep the current IF contents and status; hold PC.
            // Do not overwrite with a bubble so the instruction is ready when stall clears.
        } else if (flush) {
            // Branch misprediction: start fetching from correct target
            bool iHit = iCache->access(PC, CACHE_READ);
            if (!iHit) {
                iMissRemaining = iCache->config.missLatency;
                iMissActive = true;
            } else {
                iMissActive = false;
            }
            // The speculative instruction is squashed
            pipelineInfo.ifInst = nop(SQUASHED);
            pipelineInfo.ifInst.PC = PC;
        } else {
            // Normal I-Cache access and timing
            if (iMissRemaining > 0) {
                // Waiting for miss to resolve
                iMissRemaining--;
                if (iMissRemaining == 0 && iMissActive) {
                    // Miss just resolved - fetch without another cache access
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    pipelineInfo.ifInst.status = NORMAL;
                    PC = PC + 4;
                    iMissActive = false;
                } else {
                    // Still waiting
                    pipelineInfo.ifInst = nop(NORMAL);
                    pipelineInfo.ifInst.PC = PC;
                }
            } else {
                // No pending miss - do normal cache access
                bool iHit = iCache->access(PC, CACHE_READ);
                if (!iHit) {
                    iMissRemaining = iCache->config.missLatency;
                    iMissActive = true;
                    pipelineInfo.ifInst = nop(NORMAL);
                    pipelineInfo.ifInst.PC = PC;
                } else {
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    pipelineInfo.ifInst.status = NORMAL;
                    PC = PC + 4;
                }
            }
        }

        // Handle memory exception detected in MEM: schedule for next cycle
        if (pipelineInfo.memInst.memException) {
            pendingFlush = true;
            pendingFlushPC = 0x8000;
        }

        // Ensure IF stage is not mislabeled once an instruction is fetched.
        if (!pipelineInfo.ifInst.isNop) {
            if (pipelineInfo.ifInst.status == BUBBLE || pipelineInfo.ifInst.status == IDLE) {
                pipelineInfo.ifInst.status = NORMAL;
            }
        }

        doneInst = pipelineInfo.wbInst;

        if (status == HALT) {
            break;
        }
    }

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
    uint64_t icHits = iCache ? iCache->getHits() : 0;
    uint64_t icMisses = iCache ? iCache->getMisses() : 0;
    uint64_t dcHits = dCache ? dCache->getHits() : 0;
    uint64_t dcMisses = dCache ? dCache->getMisses() : 0;
    SimulationStats stats{committedDin, cycleCount, icHits, icMisses, dcHits, dcMisses, loadUseStallCount};
    dumpSimStats(stats, output);
    simulator->dumpRegMem(output);
    return SUCCESS;
}
