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
// Previous data cache access tracking (to avoid re-accessing for same address)
static bool prevDValid = false;
static uint64_t prevDAddr = 0;
static CacheOperation prevDOp = CACHE_READ;
// Previous instruction cache PC
static uint64_t prevICPC = (uint64_t)-1;
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
    prevDValid = false;
    prevDAddr = 0;
    prevDOp = CACHE_READ;
    prevICPC = (uint64_t)-1;
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

        // Use previous cycle's pipeline state for hazard detection (like cycleWorks.cpp)
        Simulator::Instruction prevIDInst = pipelineInfo.idInst;
        Simulator::Instruction prevEXInst = pipelineInfo.exInst;
        Simulator::Instruction prevMEMInst = pipelineInfo.memInst;

        bool stall = false;
        bool flush = false;  // Branch flush (immediate effect)
        bool branchStall = false;
        // memStall is true if D-cache miss is active
        bool memStall = dMissActive;

        // Check load-use stalls (no need to stall if the destination register is x0)
        // Load in EX, dependent instruction in ID
        bool loadUseStallTriggered = false;
        if (prevEXInst.readsMem && prevEXInst.rd != 0 && !prevEXInst.isNop && prevEXInst.isLegal &&
            prevEXInst.status != BUBBLE && prevEXInst.status != SQUASHED) {
            bool hazardRs1 = (prevEXInst.rd == prevIDInst.rs1 && prevIDInst.readsRs1);
            bool hazardRs2 = (prevEXInst.rd == prevIDInst.rs2 && prevIDInst.readsRs2);
            if (hazardRs1 || hazardRs2) {
                // Special-case: load -> store data (rs2) should NOT stall; forward WB->MEM
                bool isStore = prevIDInst.writesMem;
                bool isOnlyStoreDataHazard = (!hazardRs1) && hazardRs2 && isStore;
                if (!isOnlyStoreDataHazard) {
                    stall = true;
                    loadUseStallTriggered = true;
                }
            }
        }

        // Check arithmetic-branch stall
        // ALU instruction in EX, branch in ID that needs the result
        if (prevEXInst.writesRd && prevEXInst.rd != 0 && !prevEXInst.isNop && !prevEXInst.readsMem &&
            (prevIDInst.opcode == OP_BRANCH || prevIDInst.opcode == OP_JALR)) {
            if ((prevIDInst.rs1 == prevEXInst.rd && prevIDInst.readsRs1) ||
                (prevIDInst.rs2 == prevEXInst.rd && prevIDInst.readsRs2)) {
                stall = true;
            }
        }

        // load-branch stall if branch in ID depends on load in MEM
        if (prevMEMInst.readsMem && prevMEMInst.writesRd && prevMEMInst.rd != 0 && !prevMEMInst.isNop &&
            (prevIDInst.opcode == OP_BRANCH || prevIDInst.opcode == OP_JALR)) {
            if ((prevMEMInst.rd == prevIDInst.rs1 && prevIDInst.readsRs1) ||
                (prevMEMInst.rd == prevIDInst.rs2 && prevIDInst.readsRs2)) {
                stall = true;
            }
        }

        // ==================== WB STAGE ====================
        // During D-cache miss, WB gets bubbles (except first cycle when previous MEM flows through)
        Simulator::Instruction prevMEM = pipelineInfo.memInst;

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
        bool newDMissThisCycle = false;

        // Determine the instruction that is in MEM stage this cycle
        // If already stalled from previous cycle, it's the latched instruction.
        Simulator::Instruction memCandidate = memStall ? latchedMemInst : prevEX;

        // Check D-cache access for the MEM instruction
        bool willAccessDCache = (memCandidate.readsMem || memCandidate.writesMem) &&
                                !memCandidate.isNop && memCandidate.isLegal &&
                                memCandidate.status != BUBBLE && memCandidate.status != SQUASHED;
        CacheOperation opType = memCandidate.writesMem ? CACHE_WRITE : CACHE_READ;

        // Check for D-cache miss - only access if address/operation changed
        if (!dMissActive && willAccessDCache) {
            if (!prevDValid || memCandidate.memAddress != prevDAddr || opType != prevDOp) {
                prevDValid = true;
                prevDAddr = memCandidate.memAddress;
                prevDOp = opType;

                if (!dCache->access(memCandidate.memAddress, opType)) {
                    dMissActive = true;
                    dMissRemaining = 0;
                    dMissIsLoad = memCandidate.readsMem;
                    latchedMemInst = memCandidate;
                    newDMissThisCycle = true;
                }
            }
        } else if (dMissActive) {
            dMissRemaining++;
        }

        // Execute MEM stage based on D-cache state
        if (dMissActive) {
            // Still waiting for cache - keep showing the latched instruction
            pipelineInfo.memInst = latchedMemInst;
            pipelineInfo.memInst.status = NORMAL;
        } else {
            // Cache hit or miss resolved - execute MEM
            pipelineInfo.memInst = simulator->simMEM(memCandidate);
            if (memCandidate.isNop && memCandidate.status == IDLE) {
                pipelineInfo.memInst = nop(IDLE);
            } else if (memCandidate.isNop && memCandidate.status == SQUASHED) {
                pipelineInfo.memInst = nop(SQUASHED);
            } else if (pipelineInfo.memInst.isNop) {
                pipelineInfo.memInst.status = BUBBLE;
            } else {
                pipelineInfo.memInst.status = NORMAL;
            }
        }

        // Update memStall for next stages - but NOT on the detection cycle
        // On detection cycle, EX and ID still advance; stall takes effect next cycle
        memStall = dMissActive && !newDMissThisCycle;


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
        } else if (stall && !memStall) {
            // During a stall, if there's a branch in ID, still resolve it
            // (forward values and compute branch outcome)
            if (prevIDInst.opcode == OP_BRANCH || prevIDInst.opcode == OP_JALR) {
                // Forward from MEM/WB to get the value needed for branch
                if (hazard(pipelineInfo.memInst, prevIDInst.rs1)) {
                    prevIDInst.op1Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                                      : pipelineInfo.memInst.arithResult;
                } else if (hazard(pipelineInfo.wbInst, prevIDInst.rs1)) {
                    prevIDInst.op1Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                                    : pipelineInfo.wbInst.arithResult;
                } else if (hazard(doneInst, prevIDInst.rs1)) {
                    prevIDInst.op1Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
                }
                if (hazard(pipelineInfo.memInst, prevIDInst.rs2)) {
                    prevIDInst.op2Val = pipelineInfo.memInst.readsMem ? pipelineInfo.memInst.memResult
                                                                      : pipelineInfo.memInst.arithResult;
                } else if (hazard(pipelineInfo.wbInst, prevIDInst.rs2)) {
                    prevIDInst.op2Val = pipelineInfo.wbInst.readsMem ? pipelineInfo.wbInst.memResult
                                                                    : pipelineInfo.wbInst.arithResult;
                } else if (hazard(doneInst, prevIDInst.rs2)) {
                    prevIDInst.op2Val = doneInst.readsMem ? doneInst.memResult : doneInst.arithResult;
                }
                pipelineInfo.idInst = simulator->simNextPCResolution(prevIDInst);

                // Check for branch flush during stall resolution
                if (!pipelineInfo.idInst.isHalt && pipelineInfo.idInst.nextPC != prevIDInst.PC + 4) {
                    flush = true;
                    PC = pipelineInfo.idInst.nextPC;
                }
            }
            // ID instruction stays the same (stalled), branch outcome updated if applicable
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
                    // No additional branch stall check here - already handled at cycle start
                    {
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
        } else if (stall || branchStall || memStall) {
            // Hold IF - but handle branch resolution during stall
            if (flush) {
                // Branch resolved during stall - update PC and clear I-cache miss
                // (The instruction currently in IF will be squashed since it's speculative)
                iMissActive = false;
                iMissRemaining = 0;
                // Mark IF instruction as squashed since we're branching away
                pipelineInfo.ifInst.status = SPECULATIVE;
            } else if (iMissRemaining > 0) {
                iMissRemaining--;
                if (iMissRemaining == 0 && iMissActive) {
                    // Miss resolved during stall - fetch the instruction so it's ready when stall clears
                    pipelineInfo.ifInst = simulator->simIF(PC);
                    pipelineInfo.ifInst.status = NORMAL;
                    PC = PC + 4;  // Advance PC since we fetched successfully
                    iMissActive = false;
                }
            }
            // If no active I-miss and no branch, keep the current IF contents and status; hold PC.
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

        // Check if D-cache miss resolves this cycle (at end of cycle processing)
        // Resolution happens when counter EXCEEDS latency-1 (so total stall = latency cycles)
        if (dMissActive && dMissRemaining > dCache->config.missLatency - 1) {
            dMissRemaining = 0;
            dMissActive = false;
            prevDValid = false;  // Reset so next access goes through
        }

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
