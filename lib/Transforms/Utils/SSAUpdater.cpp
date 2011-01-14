//===- SSAUpdater.cpp - Unstructured SSA Update Tool ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SSAUpdater class.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "ssaupdater"
#include "llvm/Instructions.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/SSAUpdaterImpl.h"
using namespace llvm;

typedef DenseMap<BasicBlock*, Value*> AvailableValsTy;
static AvailableValsTy &getAvailableVals(void *AV) {
  return *static_cast<AvailableValsTy*>(AV);
}

SSAUpdater::SSAUpdater(SmallVectorImpl<PHINode*> *NewPHI)
  : AV(0), ProtoType(0), ProtoName(), InsertedPHIs(NewPHI) {}

SSAUpdater::~SSAUpdater() {
  delete &getAvailableVals(AV);
}

/// Initialize - Reset this object to get ready for a new set of SSA
/// updates with type 'Ty'.  PHI nodes get a name based on 'Name'.
void SSAUpdater::Initialize(const Type *Ty, StringRef Name) {
  if (AV == 0)
    AV = new AvailableValsTy();
  else
    getAvailableVals(AV).clear();
  ProtoType = Ty;
  ProtoName = Name;
}

/// HasValueForBlock - Return true if the SSAUpdater already has a value for
/// the specified block.
bool SSAUpdater::HasValueForBlock(BasicBlock *BB) const {
  return getAvailableVals(AV).count(BB);
}

/// AddAvailableValue - Indicate that a rewritten value is available in the
/// specified block with the specified value.
void SSAUpdater::AddAvailableValue(BasicBlock *BB, Value *V) {
  assert(ProtoType != 0 && "Need to initialize SSAUpdater");
  assert(ProtoType == V->getType() &&
         "All rewritten values must have the same type");
  getAvailableVals(AV)[BB] = V;
}

/// IsEquivalentPHI - Check if PHI has the same incoming value as specified
/// in ValueMapping for each predecessor block.
static bool IsEquivalentPHI(PHINode *PHI,
                            DenseMap<BasicBlock*, Value*> &ValueMapping) {
  unsigned PHINumValues = PHI->getNumIncomingValues();
  if (PHINumValues != ValueMapping.size())
    return false;

  // Scan the phi to see if it matches.
  for (unsigned i = 0, e = PHINumValues; i != e; ++i)
    if (ValueMapping[PHI->getIncomingBlock(i)] !=
        PHI->getIncomingValue(i)) {
      return false;
    }

  return true;
}

/// GetValueAtEndOfBlock - Construct SSA form, materializing a value that is
/// live at the end of the specified block.
Value *SSAUpdater::GetValueAtEndOfBlock(BasicBlock *BB) {
  Value *Res = GetValueAtEndOfBlockInternal(BB);
  return Res;
}

/// GetValueInMiddleOfBlock - Construct SSA form, materializing a value that
/// is live in the middle of the specified block.
///
/// GetValueInMiddleOfBlock is the same as GetValueAtEndOfBlock except in one
/// important case: if there is a definition of the rewritten value after the
/// 'use' in BB.  Consider code like this:
///
///      X1 = ...
///   SomeBB:
///      use(X)
///      X2 = ...
///      br Cond, SomeBB, OutBB
///
/// In this case, there are two values (X1 and X2) added to the AvailableVals
/// set by the client of the rewriter, and those values are both live out of
/// their respective blocks.  However, the use of X happens in the *middle* of
/// a block.  Because of this, we need to insert a new PHI node in SomeBB to
/// merge the appropriate values, and this value isn't live out of the block.
///
Value *SSAUpdater::GetValueInMiddleOfBlock(BasicBlock *BB) {
  // If there is no definition of the renamed variable in this block, just use
  // GetValueAtEndOfBlock to do our work.
  if (!HasValueForBlock(BB))
    return GetValueAtEndOfBlock(BB);

  // Otherwise, we have the hard case.  Get the live-in values for each
  // predecessor.
  SmallVector<std::pair<BasicBlock*, Value*>, 8> PredValues;
  Value *SingularValue = 0;

  // We can get our predecessor info by walking the pred_iterator list, but it
  // is relatively slow.  If we already have PHI nodes in this block, walk one
  // of them to get the predecessor list instead.
  if (PHINode *SomePhi = dyn_cast<PHINode>(BB->begin())) {
    for (unsigned i = 0, e = SomePhi->getNumIncomingValues(); i != e; ++i) {
      BasicBlock *PredBB = SomePhi->getIncomingBlock(i);
      Value *PredVal = GetValueAtEndOfBlock(PredBB);
      PredValues.push_back(std::make_pair(PredBB, PredVal));

      // Compute SingularValue.
      if (i == 0)
        SingularValue = PredVal;
      else if (PredVal != SingularValue)
        SingularValue = 0;
    }
  } else {
    bool isFirstPred = true;
    for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
      BasicBlock *PredBB = *PI;
      Value *PredVal = GetValueAtEndOfBlock(PredBB);
      PredValues.push_back(std::make_pair(PredBB, PredVal));

      // Compute SingularValue.
      if (isFirstPred) {
        SingularValue = PredVal;
        isFirstPred = false;
      } else if (PredVal != SingularValue)
        SingularValue = 0;
    }
  }

  // If there are no predecessors, just return undef.
  if (PredValues.empty())
    return UndefValue::get(ProtoType);

  // Otherwise, if all the merged values are the same, just use it.
  if (SingularValue != 0)
    return SingularValue;

  // Otherwise, we do need a PHI: check to see if we already have one available
  // in this block that produces the right value.
  if (isa<PHINode>(BB->begin())) {
    DenseMap<BasicBlock*, Value*> ValueMapping(PredValues.begin(),
                                               PredValues.end());
    PHINode *SomePHI;
    for (BasicBlock::iterator It = BB->begin();
         (SomePHI = dyn_cast<PHINode>(It)); ++It) {
      if (IsEquivalentPHI(SomePHI, ValueMapping))
        return SomePHI;
    }
  }

  // Ok, we have no way out, insert a new one now.
  PHINode *InsertedPHI = PHINode::Create(ProtoType, ProtoName, &BB->front());
  InsertedPHI->reserveOperandSpace(PredValues.size());

  // Fill in all the predecessors of the PHI.
  for (unsigned i = 0, e = PredValues.size(); i != e; ++i)
    InsertedPHI->addIncoming(PredValues[i].second, PredValues[i].first);

  // See if the PHI node can be merged to a single value.  This can happen in
  // loop cases when we get a PHI of itself and one other value.
  if (Value *V = SimplifyInstruction(InsertedPHI)) {
    InsertedPHI->eraseFromParent();
    return V;
  }

  // If the client wants to know about all new instructions, tell it.
  if (InsertedPHIs) InsertedPHIs->push_back(InsertedPHI);

  DEBUG(dbgs() << "  Inserted PHI: " << *InsertedPHI << "\n");
  return InsertedPHI;
}

/// RewriteUse - Rewrite a use of the symbolic value.  This handles PHI nodes,
/// which use their value in the corresponding predecessor.
void SSAUpdater::RewriteUse(Use &U) {
  Instruction *User = cast<Instruction>(U.getUser());

  Value *V;
  if (PHINode *UserPN = dyn_cast<PHINode>(User))
    V = GetValueAtEndOfBlock(UserPN->getIncomingBlock(U));
  else
    V = GetValueInMiddleOfBlock(User->getParent());

  U.set(V);
}

/// RewriteUseAfterInsertions - Rewrite a use, just like RewriteUse.  However,
/// this version of the method can rewrite uses in the same block as a
/// definition, because it assumes that all uses of a value are below any
/// inserted values.
void SSAUpdater::RewriteUseAfterInsertions(Use &U) {
  Instruction *User = cast<Instruction>(U.getUser());
  
  Value *V;
  if (PHINode *UserPN = dyn_cast<PHINode>(User))
    V = GetValueAtEndOfBlock(UserPN->getIncomingBlock(U));
  else
    V = GetValueAtEndOfBlock(User->getParent());
  
  U.set(V);
}

/// PHIiter - Iterator for PHI operands.  This is used for the PHI_iterator
/// in the SSAUpdaterImpl template.
namespace {
  class PHIiter {
  private:
    PHINode *PHI;
    unsigned idx;

  public:
    explicit PHIiter(PHINode *P) // begin iterator
      : PHI(P), idx(0) {}
    PHIiter(PHINode *P, bool) // end iterator
      : PHI(P), idx(PHI->getNumIncomingValues()) {}

    PHIiter &operator++() { ++idx; return *this; } 
    bool operator==(const PHIiter& x) const { return idx == x.idx; }
    bool operator!=(const PHIiter& x) const { return !operator==(x); }
    Value *getIncomingValue() { return PHI->getIncomingValue(idx); }
    BasicBlock *getIncomingBlock() { return PHI->getIncomingBlock(idx); }
  };
}

/// SSAUpdaterTraits<SSAUpdater> - Traits for the SSAUpdaterImpl template,
/// specialized for SSAUpdater.
namespace llvm {
template<>
class SSAUpdaterTraits<SSAUpdater> {
public:
  typedef BasicBlock BlkT;
  typedef Value *ValT;
  typedef PHINode PhiT;

  typedef succ_iterator BlkSucc_iterator;
  static BlkSucc_iterator BlkSucc_begin(BlkT *BB) { return succ_begin(BB); }
  static BlkSucc_iterator BlkSucc_end(BlkT *BB) { return succ_end(BB); }

  typedef PHIiter PHI_iterator;
  static inline PHI_iterator PHI_begin(PhiT *PHI) { return PHI_iterator(PHI); }
  static inline PHI_iterator PHI_end(PhiT *PHI) {
    return PHI_iterator(PHI, true);
  }

  /// FindPredecessorBlocks - Put the predecessors of Info->BB into the Preds
  /// vector, set Info->NumPreds, and allocate space in Info->Preds.
  static void FindPredecessorBlocks(BasicBlock *BB,
                                    SmallVectorImpl<BasicBlock*> *Preds) {
    // We can get our predecessor info by walking the pred_iterator list,
    // but it is relatively slow.  If we already have PHI nodes in this
    // block, walk one of them to get the predecessor list instead.
    if (PHINode *SomePhi = dyn_cast<PHINode>(BB->begin())) {
      for (unsigned PI = 0, E = SomePhi->getNumIncomingValues(); PI != E; ++PI)
        Preds->push_back(SomePhi->getIncomingBlock(PI));
    } else {
      for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI)
        Preds->push_back(*PI);
    }
  }

  /// GetUndefVal - Get an undefined value of the same type as the value
  /// being handled.
  static Value *GetUndefVal(BasicBlock *BB, SSAUpdater *Updater) {
    return UndefValue::get(Updater->ProtoType);
  }

  /// CreateEmptyPHI - Create a new PHI instruction in the specified block.
  /// Reserve space for the operands but do not fill them in yet.
  static Value *CreateEmptyPHI(BasicBlock *BB, unsigned NumPreds,
                               SSAUpdater *Updater) {
    PHINode *PHI = PHINode::Create(Updater->ProtoType, Updater->ProtoName,
                                   &BB->front());
    PHI->reserveOperandSpace(NumPreds);
    return PHI;
  }

  /// AddPHIOperand - Add the specified value as an operand of the PHI for
  /// the specified predecessor block.
  static void AddPHIOperand(PHINode *PHI, Value *Val, BasicBlock *Pred) {
    PHI->addIncoming(Val, Pred);
  }

  /// InstrIsPHI - Check if an instruction is a PHI.
  ///
  static PHINode *InstrIsPHI(Instruction *I) {
    return dyn_cast<PHINode>(I);
  }

  /// ValueIsPHI - Check if a value is a PHI.
  ///
  static PHINode *ValueIsPHI(Value *Val, SSAUpdater *Updater) {
    return dyn_cast<PHINode>(Val);
  }

  /// ValueIsNewPHI - Like ValueIsPHI but also check if the PHI has no source
  /// operands, i.e., it was just added.
  static PHINode *ValueIsNewPHI(Value *Val, SSAUpdater *Updater) {
    PHINode *PHI = ValueIsPHI(Val, Updater);
    if (PHI && PHI->getNumIncomingValues() == 0)
      return PHI;
    return 0;
  }

  /// GetPHIValue - For the specified PHI instruction, return the value
  /// that it defines.
  static Value *GetPHIValue(PHINode *PHI) {
    return PHI;
  }
};

} // End llvm namespace

/// GetValueAtEndOfBlockInternal - Check to see if AvailableVals has an entry
/// for the specified BB and if so, return it.  If not, construct SSA form by
/// first calculating the required placement of PHIs and then inserting new
/// PHIs where needed.
Value *SSAUpdater::GetValueAtEndOfBlockInternal(BasicBlock *BB) {
  AvailableValsTy &AvailableVals = getAvailableVals(AV);
  if (Value *V = AvailableVals[BB])
    return V;

  SSAUpdaterImpl<SSAUpdater> Impl(this, &AvailableVals, InsertedPHIs);
  return Impl.GetValue(BB);
}

//===----------------------------------------------------------------------===//
// LoadAndStorePromoter Implementation
//===----------------------------------------------------------------------===//

void LoadAndStorePromoter::run(StringRef BaseName,
                               const SmallVectorImpl<Instruction*> &Insts,
                               SSAUpdater *SSA) {
  if (Insts.empty()) return;
  
  // If no SSAUpdater was provided, use a default one.  This allows the client
  // to capture inserted PHI nodes etc if they want.
  SSAUpdater DefaultSSA;
  if (SSA == 0) SSA = &DefaultSSA;
  
  const Type *ValTy;
  if (LoadInst *LI = dyn_cast<LoadInst>(Insts[0]))
    ValTy = LI->getType();
  else
    ValTy = cast<StoreInst>(Insts[0])->getOperand(0)->getType();
    
  SSA->Initialize(ValTy, BaseName);
  
  // First step: bucket up uses of the alloca by the block they occur in.
  // This is important because we have to handle multiple defs/uses in a block
  // ourselves: SSAUpdater is purely for cross-block references.
  // FIXME: Want a TinyVector<Instruction*> since there is often 0/1 element.
  DenseMap<BasicBlock*, std::vector<Instruction*> > UsesByBlock;
  
  for (unsigned i = 0, e = Insts.size(); i != e; ++i) {
    Instruction *User = Insts[i];
    UsesByBlock[User->getParent()].push_back(User);
  }
  
  // Okay, now we can iterate over all the blocks in the function with uses,
  // processing them.  Keep track of which loads are loading a live-in value.
  // Walk the uses in the use-list order to be determinstic.
  SmallVector<LoadInst*, 32> LiveInLoads;
  DenseMap<Value*, Value*> ReplacedLoads;
  
  for (unsigned i = 0, e = Insts.size(); i != e; ++i) {
    Instruction *User = Insts[i];
    BasicBlock *BB = User->getParent();
    std::vector<Instruction*> &BlockUses = UsesByBlock[BB];
    
    // If this block has already been processed, ignore this repeat use.
    if (BlockUses.empty()) continue;
    
    // Okay, this is the first use in the block.  If this block just has a
    // single user in it, we can rewrite it trivially.
    if (BlockUses.size() == 1) {
      // If it is a store, it is a trivial def of the value in the block.
      if (StoreInst *SI = dyn_cast<StoreInst>(User))
        SSA->AddAvailableValue(BB, SI->getOperand(0));
      else 
        // Otherwise it is a load, queue it to rewrite as a live-in load.
        LiveInLoads.push_back(cast<LoadInst>(User));
      BlockUses.clear();
      continue;
    }
    
    // Otherwise, check to see if this block is all loads.
    bool HasStore = false;
    for (unsigned i = 0, e = BlockUses.size(); i != e; ++i) {
      if (isa<StoreInst>(BlockUses[i])) {
        HasStore = true;
        break;
      }
    }
    
    // If so, we can queue them all as live in loads.  We don't have an
    // efficient way to tell which on is first in the block and don't want to
    // scan large blocks, so just add all loads as live ins.
    if (!HasStore) {
      for (unsigned i = 0, e = BlockUses.size(); i != e; ++i)
        LiveInLoads.push_back(cast<LoadInst>(BlockUses[i]));
      BlockUses.clear();
      continue;
    }
    
    // Otherwise, we have mixed loads and stores (or just a bunch of stores).
    // Since SSAUpdater is purely for cross-block values, we need to determine
    // the order of these instructions in the block.  If the first use in the
    // block is a load, then it uses the live in value.  The last store defines
    // the live out value.  We handle this by doing a linear scan of the block.
    Value *StoredValue = 0;
    for (BasicBlock::iterator II = BB->begin(), E = BB->end(); II != E; ++II) {
      if (LoadInst *L = dyn_cast<LoadInst>(II)) {
        // If this is a load from an unrelated pointer, ignore it.
        if (!isInstInList(L, Insts)) continue;
        
        // If we haven't seen a store yet, this is a live in use, otherwise
        // use the stored value.
        if (StoredValue) {
          L->replaceAllUsesWith(StoredValue);
          ReplacedLoads[L] = StoredValue;
        } else {
          LiveInLoads.push_back(L);
        }
        continue;
      }
      
      if (StoreInst *S = dyn_cast<StoreInst>(II)) {
        // If this is a store to an unrelated pointer, ignore it.
        if (!isInstInList(S, Insts)) continue;
        
        // Remember that this is the active value in the block.
        StoredValue = S->getOperand(0);
      }
    }
    
    // The last stored value that happened is the live-out for the block.
    assert(StoredValue && "Already checked that there is a store in block");
    SSA->AddAvailableValue(BB, StoredValue);
    BlockUses.clear();
  }
  
  // Okay, now we rewrite all loads that use live-in values in the loop,
  // inserting PHI nodes as necessary.
  for (unsigned i = 0, e = LiveInLoads.size(); i != e; ++i) {
    LoadInst *ALoad = LiveInLoads[i];
    Value *NewVal = SSA->GetValueInMiddleOfBlock(ALoad->getParent());
    ALoad->replaceAllUsesWith(NewVal);
    ReplacedLoads[ALoad] = NewVal;
  }
  
  // Now that everything is rewritten, delete the old instructions from the
  // function.  They should all be dead now.
  for (unsigned i = 0, e = Insts.size(); i != e; ++i) {
    Instruction *User = Insts[i];
    
    // If this is a load that still has uses, then the load must have been added
    // as a live value in the SSAUpdate data structure for a block (e.g. because
    // the loaded value was stored later).  In this case, we need to recursively
    // propagate the updates until we get to the real value.
    if (!User->use_empty()) {
      Value *NewVal = ReplacedLoads[User];
      assert(NewVal && "not a replaced load?");
      
      // Propagate down to the ultimate replacee.  The intermediately loads
      // could theoretically already have been deleted, so we don't want to
      // dereference the Value*'s.
      DenseMap<Value*, Value*>::iterator RLI = ReplacedLoads.find(NewVal);
      while (RLI != ReplacedLoads.end()) {
        NewVal = RLI->second;
        RLI = ReplacedLoads.find(NewVal);
      }
      
      User->replaceAllUsesWith(NewVal);
    }
    
    User->eraseFromParent();
  }
}
