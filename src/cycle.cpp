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

static uint64_t PC = 0;

/**TODO: Implement pipeline simulation for the RISCV machine in this file.
 * A basic template is provided below that doesn't account for any hazards.
 */


Simulator::Instruction doneInst;

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013;
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
    doneInst = nop(IDLE);
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
    PipeState pipeState = {
        0,
    };


    while (cycles == 0 || count < cycles) {

        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        Simulator::Instruction ID = pipelineInfo.idInst;
        Simulator::Instruction EX = pipelineInfo.exInst;

        bool stall = false;
        bool flush = false;
        bool branchStall = false;

        // Check load-use stalls (no need to stall if the destination register is x0)


        if (EX.readsMem && EX.rd != 0) {
            if (EX.rd == ID.rs1 || EX.rd == ID.rs2) {
            stall = true;
            }
        }


        // Check arithmetic-branch stall (including load)
        if (EX.writesRd && (ID.opcode == OP_BRANCH || ID.opcode == OP_JALR) && EX.rd != 0) {
            if (ID.rs1 == EX.rd || ID.rs2 == EX.rd) {
                stall = true;
            }
        }


        // load-branch stall number 2 maybe?
        if (pipelineInfo.memInst.readsMem && pipelineInfo.memInst.writesRd && (ID.opcode == OP_BRANCH || ID.opcode == OP_JALR) && pipelineInfo.memInst.rd != 0) {
            if (pipelineInfo.memInst.rd == ID.rs1 || pipelineInfo.memInst.rd == ID.rs2) {
            stall = true;
            }
        }

        // TAKE CARE OF WB
        pipelineInfo.wbInst = simulator->simWB(pipelineInfo.memInst);
        pipelineInfo.wbInst.status = NORMAL; // Not sure yet whether this is correct

        if (pipelineInfo.wbInst.isHalt) {
            status = HALT;
        }

        // TAKE CARE OF MEM

        // Need to do some forwarding stuff here I think

        // Do a WB to MEM forward to give load to a store (load-store forwarding)
        if (pipelineInfo.exInst.writesMem && pipelineInfo.wbInst.rd != 0 && pipelineInfo.wbInst.readsMem) {
            if (pipelineInfo.exInst.rs2 == pipelineInfo.wbInst.rd) {
                pipelineInfo.exInst.op2Val = pipelineInfo.wbInst.memResult;
            }
        }


        pipelineInfo.memInst = simulator->simMEM(pipelineInfo.exInst);
        if (pipelineInfo.memInst.isHalt || pipelineInfo.memInst.isNop) {
            pipelineInfo.memInst.status = BUBBLE;
        } else {
            pipelineInfo.memInst.status = NORMAL;
        }

        // Take care of EX
        // Now we have to start worrying about stalls
        if (stall) {
            pipelineInfo.exInst = nop(BUBBLE);
        } else {
            // Take care of forwarding to the stage about to run EX

            // Register Source 1
            if (hazard(pipelineInfo.memInst, pipelineInfo.idInst.rs1)) {
                if (pipelineInfo.memInst.readsMem) {
                    pipelineInfo.idInst.op1Val = pipelineInfo.memInst.memResult;
                } else {
                    pipelineInfo.idInst.op1Val = pipelineInfo.memInst.arithResult;
                }
            } else if(hazard(pipelineInfo.wbInst, pipelineInfo.idInst.rs1)) {
                if (pipelineInfo.wbInst.readsMem) {
                    pipelineInfo.idInst.op1Val = pipelineInfo.wbInst.memResult;
                } else {
                    pipelineInfo.idInst.op1Val = pipelineInfo.wbInst.arithResult;
                }
            } else if (hazard(doneInst, pipelineInfo.idInst.rs1)) {
                if (doneInst.readsMem) {
                    pipelineInfo.idInst.op1Val = doneInst.memResult;
                } else {
                    pipelineInfo.idInst.op1Val = doneInst.arithResult;
                }
            }
            

            // Register Source 2
            if (hazard(pipelineInfo.memInst, pipelineInfo.idInst.rs2)) {
                if (pipelineInfo.memInst.readsMem) {
                    pipelineInfo.idInst.op2Val = pipelineInfo.memInst.memResult;
                } else {
                    pipelineInfo.idInst.op2Val = pipelineInfo.memInst.arithResult;
                }
            } else if(hazard(pipelineInfo.wbInst, pipelineInfo.idInst.rs2)) {
                if (pipelineInfo.wbInst.readsMem) {
                    pipelineInfo.idInst.op2Val = pipelineInfo.wbInst.memResult;
                } else {
                    pipelineInfo.idInst.op2Val = pipelineInfo.wbInst.arithResult;
                }
            } else if (hazard(doneInst, pipelineInfo.idInst.rs2)) {
                if (doneInst.readsMem) {
                    pipelineInfo.idInst.op2Val = doneInst.memResult;
                } else {
                    pipelineInfo.idInst.op2Val = doneInst.arithResult;
                }
            }




            pipelineInfo.exInst = simulator->simEX(pipelineInfo.idInst);
            if (pipelineInfo.exInst.isNop || pipelineInfo.exInst.isHalt) {
                pipelineInfo.exInst.status = BUBBLE;
            } else {
                pipelineInfo.exInst.status = NORMAL;
            }
        }

        // Take care of ID
        if (stall) {
            // Don't put a bubble here (I think), just hold the instruction

        } else {
            Simulator::Instruction newIDInst = simulator->simID(pipelineInfo.ifInst); // don't directly write to pipelineInfo.idInst yet

            // Take care of branch forwarding 
            // Need to make sure to delay if needed branch values are not ready yet
            if (newIDInst.opcode == OP_BRANCH || newIDInst.opcode == OP_JALR) {
                if (hazard(pipelineInfo.exInst, newIDInst.rs1) || hazard(pipelineInfo.exInst, newIDInst.rs2)) {
                    branchStall = true;
                }
                if (hazard(pipelineInfo.memInst, newIDInst.rs1) || hazard(pipelineInfo.memInst, newIDInst.rs2)) {
                    if (pipelineInfo.memInst.readsMem) {
                        branchStall = true;
                    }
                }
            }

            if (branchStall) {
                pipelineInfo.idInst = nop(BUBBLE);
            } else {
                if (newIDInst.opcode == OP_BRANCH || newIDInst.opcode == OP_JALR) {
                    if (hazard(pipelineInfo.memInst, newIDInst.rs1)) {
                        if (pipelineInfo.memInst.readsMem) {
                            newIDInst.op1Val = pipelineInfo.memInst.memResult;
                        } else {
                            newIDInst.op1Val = pipelineInfo.memInst.arithResult;
                        }
                    } else if (hazard(pipelineInfo.wbInst, newIDInst.rs1)) {
                        if (pipelineInfo.wbInst.readsMem) {
                            newIDInst.op1Val = pipelineInfo.wbInst.memResult;
                        } else {
                            newIDInst.op1Val = pipelineInfo.wbInst.arithResult;
                        }
                    } else if (hazard(doneInst, newIDInst.rs1)) {
                        if (doneInst.readsMem) {
                            newIDInst.op1Val = doneInst.memResult;
                        } else {
                            newIDInst.op1Val = doneInst.arithResult;
                        }
                    }

                    // Register Source 2
                    if (hazard(pipelineInfo.memInst, newIDInst.rs2)) {
                        if (pipelineInfo.memInst.readsMem) {
                            newIDInst.op2Val = pipelineInfo.memInst.memResult;
                        } else {
                            newIDInst.op2Val = pipelineInfo.memInst.arithResult;
                        }
                    } else if (hazard(pipelineInfo.wbInst, newIDInst.rs2)) {
                        if (pipelineInfo.wbInst.readsMem) {
                            newIDInst.op2Val = pipelineInfo.wbInst.memResult;
                        } else {
                            newIDInst.op2Val = pipelineInfo.wbInst.arithResult;
                        }
                    } else if (hazard(doneInst, newIDInst.rs2)) {
                        if (doneInst.readsMem) {
                            newIDInst.op2Val = doneInst.memResult;
                        } else {
                            newIDInst.op2Val = doneInst.arithResult;
                        }
                    }

                    newIDInst = simulator->simNextPCResolution(newIDInst);
                }
                pipelineInfo.idInst = newIDInst;
                if (pipelineInfo.idInst.isNop || pipelineInfo.idInst.isHalt) {
                    pipelineInfo.idInst.status = BUBBLE;
                } else {
                    pipelineInfo.idInst.status = NORMAL;
                }

                 if (pipelineInfo.idInst.nextPC != pipelineInfo.ifInst.PC + 4) {
                    flush = true;
                    PC = pipelineInfo.idInst.nextPC;
                }

            }
        }


        // Take care of IF
        if (stall || branchStall) {
            // Do nothing, hold instruction in IF

        } else if (flush) {

            pipelineInfo.ifInst = nop(BUBBLE);

        } else {
            pipelineInfo.ifInst = simulator->simIF(PC);
            pipelineInfo.ifInst.status = NORMAL;
            PC = PC + 4;
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
    simulator->dumpRegMem(output);
    SimulationStats stats{simulator->getDin(),  cycleCount, 0, 0, 0, 0, 0};  // TODO incomplete implementation
    dumpSimStats(stats, output);
    return SUCCESS;
}
