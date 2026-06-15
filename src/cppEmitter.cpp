/*
  cppEmitter: emit C++ files for simulation
*/

#include "common.h"
#include "util.h"

#include <cstddef>
#include <cstdio>
#include <cinttypes>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#define ACTIVE_WIDTH 8
#define RESET_PER_FUNC 400
#define MT_PURE_BATCH_SHARD_SIZE 256

#define ENABLE_ACTIVATOR false

#ifdef DIFFTEST_PER_SIG
FILE* sigFile = nullptr;
#endif

#define RESET_NAME(node) (node->name + "$RESET")
#define emitFuncDecl(indent, ...) __emitSrc(indent, true, true, NULL, __VA_ARGS__)
#define emitBodyLock(indent, ...) __emitSrc(indent, false, false, NULL, __VA_ARGS__)

#define ActiveType std::tuple<uint64_t, std::string, int>
#define ACTIVE_MASK(active) std::get<0>(active)
#define ACTIVE_COMMENT(active) std::get<1>(active)
#define ACTIVE_UNIQUE(active) std::get<2>(active)

static int superId = 0;
static int activeFlagNum = 0;
static std::set<Node*> definedNode;
static std::map<int, SuperNode*> cppId2Super;
static std::set<int> alwaysActive;

static std::map<Node*, std::pair<int, int>> super2ResetId;  // uint & async reset

extern int maxConcatNum;
bool nameExist(std::string str);
static int resetFuncNum = 0;
std::pair<int, uint64_t> setIdxMask(int cppId);

static bool isAlwaysActive(int cppId) {
  return alwaysActive.find(cppId) != alwaysActive.end();
}

static bool hasCppId(const std::set<SuperNode*>& supers, int cppId) {
  for (SuperNode* super : supers) {
    if (super && super->cppId == cppId) return true;
  }
  return false;
}

struct MtBoundaryInfo {
  std::map<std::string, int> nodeKinds;
  std::set<std::string> clockNames;
  std::set<std::string> stateTargetNames;
  std::set<std::string> rhsReadStateTargetNames;
  bool hasStateUpdate = false;
  bool hasAmbiguousStateTarget = false;
  bool hasRhsNextStateObjectRead = false;
  bool hasUnexpandedRhsDependency = false;
  bool hasMemoryWrite = false;
  bool hasMemoryRead = false;
  bool hasReset = false;
  bool hasAsyncReset = false;
  bool hasActivateAllPath = false;
  bool hasExternal = false;
  bool hasSpecial = false;
  bool hasUnknownNode = false;
  bool hasUnknownOp = false;
  bool hasArrayOrDynamicIndex = false;
  int stateSourceCommitCount = 0;
  int stateNextUpdateCount = 0;
  int stateResetUpdateCount = 0;
};

struct MtTaskInfo {
  MtBoundaryInfo boundary;
  std::string taskKind;
  std::vector<std::string> serialReasons;
  bool isSource = false;
  bool isSink = false;
  int candidateCost = 0;
  bool hasCandidateCost = false;
  std::string repcutRole = "none";
  int repcutSourceCount = 0;
  int repcutSinkCount = 0;
  int repcutFanout = 0;
  int repcutCopyCost = 0;
  std::string repcutBlockReason;
  bool repcutSelected = false;
  bool repcutRuntimeApplied = false;
  int repcutCutInEdges = 0;
  int repcutCutOutEdges = 0;
};

struct MtRepCutEdge {
  int fromCppId = -1;
  int toCppId = -1;
  std::string reason;
};

struct MtPureBatchPlan {
  std::vector<std::pair<int, int>> batches;
  std::vector<MtRepCutEdge> cutEdges;
  int segmentCount = 0;
};

struct MtCoarseLayer {
  std::vector<int> taskCppIds;
};

struct MtCoarseMTask {
  std::vector<std::vector<int>> layerTaskCppIds;
  int taskCount = 0;
  int orderingEdgeCount = 0;
};

struct MtCoarseRegion {
  int beginCppId = -1;
  int endCppId = -1;
  int beginActiveWord = -1;
  int endActiveWord = -1;
  int taskCount = 0;
  int activeWordSpan = 0;
  int staticCost = 0;
  int memberNodeCost = 0;
  int expectedActiveCost = 0;
  int pureTaskCount = 0;
  int serialBlockerCount = 0;
  int dependencyEdgeCount = 0;
  int activeVisibilityEdgeCount = 0;
  int sameCycleActivationHazardCount = 0;
  int replicationCandidateCount = 0;
  int estimatedLayerCount = 0;
  int estimatedMaxParallelWidth = 0;
  bool runtimeEligible = false;
  bool repcutLiteCouldHelp = false;
  std::vector<std::string> blockers;
  std::vector<MtCoarseLayer> layers;
  std::vector<MtCoarseMTask> mtasks;
};

struct MtCoarseRegionPlan {
  std::vector<MtCoarseRegion> regions;
};

struct MtCoarseProfileFacts {
  int runtimeEligibleRegionCount = 0;
  int runtimeLayerCount = 0;
  int maxRegionLayerCount = 0;
  int runtimeMTaskCount = 0;
  int layerSizeHist[6] = {0, 0, 0, 0, 0, 0};
  int regionLayerCountHist[6] = {0, 0, 0, 0, 0, 0};
};

struct MtRepCutClone {
  int sourceCppId = -1;
  int sinkCppId = -1;
  Node* sourceNode = nullptr;
  std::string cloneName;
  std::string expr;
  std::string fallbackReason;
};

struct MtRepCutBatch {
  int beginCppId = -1;
  int endCppId = -1;
  int cutEdgeCount = 0;
  int cloneCount = 0;
  uint64_t forcedSinkMask = 0;
  std::set<int> forcedSinkCppIds;
  bool forcedSerial = true;
  bool parallelSafe = false;
  bool forcedSinkActivation = false;
  std::string parallelSafeReason;
  std::string fallbackReason;
};

struct MtRepCutSemanticPlan {
  MtPureBatchPlan batchPlan;
  std::vector<MtRepCutClone> clones;
  std::vector<MtRepCutBatch> cutBatches;
};

static const char* nodeTypeName(NodeType type) {
  switch (type) {
    case NODE_INVALID: return "NODE_INVALID";
    case NODE_REG_SRC: return "NODE_REG_SRC";
    case NODE_REG_DST: return "NODE_REG_DST";
    case NODE_SPECIAL: return "NODE_SPECIAL";
    case NODE_INP: return "NODE_INP";
    case NODE_OUT: return "NODE_OUT";
    case NODE_MEMORY: return "NODE_MEMORY";
    case NODE_READER: return "NODE_READER";
    case NODE_WRITER: return "NODE_WRITER";
    case NODE_READWRITER: return "NODE_READWRITER";
    case NODE_INFER: return "NODE_INFER";
    case NODE_OTHERS: return "NODE_OTHERS";
    case NODE_REG_RESET: return "NODE_REG_RESET";
    case NODE_EXT_IN: return "NODE_EXT_IN";
    case NODE_EXT_OUT: return "NODE_EXT_OUT";
    case NODE_EXT: return "NODE_EXT";
  }
  return "NODE_UNKNOWN";
}

static bool isKnownNodeType(NodeType type) {
  switch (type) {
    case NODE_INVALID:
    case NODE_REG_SRC:
    case NODE_REG_DST:
    case NODE_SPECIAL:
    case NODE_INP:
    case NODE_OUT:
    case NODE_MEMORY:
    case NODE_READER:
    case NODE_WRITER:
    case NODE_READWRITER:
    case NODE_INFER:
    case NODE_OTHERS:
    case NODE_REG_RESET:
    case NODE_EXT_IN:
    case NODE_EXT_OUT:
    case NODE_EXT:
      return true;
  }
  return false;
}

static const char* superTypeName(SuperType type) {
  switch (type) {
    case SUPER_VALID: return "SUPER_VALID";
    case SUPER_EXTMOD: return "SUPER_EXTMOD";
    case SUPER_ASYNC_RESET: return "SUPER_ASYNC_RESET";
    case SUPER_UINT_RESET: return "SUPER_UINT_RESET";
    case SUPER_UPDATE_REG: return "SUPER_UPDATE_REG";
  }
  return "SUPER_UNKNOWN";
}

static std::string jsonEscape(const std::string& str) {
  std::string ret;
  for (char ch : str) {
    switch (ch) {
      case '\\': ret += "\\\\"; break;
      case '"': ret += "\\\""; break;
      case '\b': ret += "\\b"; break;
      case '\f': ret += "\\f"; break;
      case '\n': ret += "\\n"; break;
      case '\r': ret += "\\r"; break;
      case '\t': ret += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) ret += format("\\u%04x", ch);
        else ret += ch;
        break;
    }
  }
  return ret;
}

static void dumpJsonIntArray(FILE* fp, const std::set<int>& values) {
  fprintf(fp, "[");
  bool first = true;
  for (int value : values) {
    if (!first) fprintf(fp, ", ");
    first = false;
    fprintf(fp, "%d", value);
  }
  fprintf(fp, "]");
}

static void dumpJsonIntArray(FILE* fp, const std::vector<int>& values) {
  fprintf(fp, "[");
  bool first = true;
  for (int value : values) {
    if (!first) fprintf(fp, ", ");
    first = false;
    fprintf(fp, "%d", value);
  }
  fprintf(fp, "]");
}

static void dumpJsonStringArray(FILE* fp, const std::set<std::string>& values) {
  fprintf(fp, "[");
  bool first = true;
  for (const std::string& value : values) {
    if (!first) fprintf(fp, ", ");
    first = false;
    fprintf(fp, "\"%s\"", jsonEscape(value).c_str());
  }
  fprintf(fp, "]");
}

static void dumpJsonStringArray(FILE* fp, const std::vector<std::string>& values) {
  fprintf(fp, "[");
  bool first = true;
  for (const std::string& value : values) {
    if (!first) fprintf(fp, ", ");
    first = false;
    fprintf(fp, "\"%s\"", jsonEscape(value).c_str());
  }
  fprintf(fp, "]");
}

static void addCppIdIfExecutable(std::set<int>& ids, SuperNode* super) {
  if (super && super->cppId >= 0) ids.insert(super->cppId);
}

static void addCppIdsIfExecutable(std::set<int>& ids, const std::set<SuperNode*>& supers) {
  for (SuperNode* super : supers) addCppIdIfExecutable(ids, super);
}

static bool nodeHasStateUpdate(Node* node) {
  return node->type == NODE_REG_DST || node->type == NODE_REG_RESET ||
         (node->type == NODE_REG_SRC && node->regNext && node->regNext->status == VALID_NODE);
}

static bool stateTargetNameForNode(Node* node, std::string& targetName) {
  if (!node) return false;
  Node* target = nullptr;
  if (node->type == NODE_REG_SRC) {
    if (!node->regNext || node->regNext->status != VALID_NODE) return false;
    target = node;
  } else if (node->type == NODE_REG_DST) {
    if (!node->regNext) return false;
    target = node->getSrc();
  } else if (node->type == NODE_REG_RESET) {
    if (!node->regNext) return false;
    target = node->getResetSrc();
  } else {
    return false;
  }
  if (!target || target->name.empty()) return false;
  targetName = target->name;
  return true;
}

static void collectStateTargetName(Node* node, MtBoundaryInfo& info) {
  if (!nodeHasStateUpdate(node)) return;
  if (node->type == NODE_REG_SRC) info.stateSourceCommitCount ++;
  else if (node->type == NODE_REG_DST) info.stateNextUpdateCount ++;
  else if (node->type == NODE_REG_RESET) info.stateResetUpdateCount ++;
  std::string targetName;
  if (stateTargetNameForNode(node, targetName)) {
    info.stateTargetNames.insert(targetName);
  } else {
    info.hasAmbiguousStateTarget = true;
  }
}

static const size_t MT_RHS_STATE_READ_ENODE_LIMIT = 128;
static const size_t MT_RHS_STATE_READ_EXPAND_NODE_LIMIT = 32;

static void collectMtRhsStateReads(ENode* root,
                                   MtBoundaryInfo& info,
                                   Node* owner,
                                   std::set<ENode*>& visitedENodes,
                                   std::set<Node*>& expandedNodes) {
  if (!root) return;
  std::stack<ENode*> stack;
  stack.push(root);
  while (!stack.empty()) {
    ENode* top = stack.top();
    stack.pop();
    if (!top) continue;
    if (visitedENodes.find(top) != visitedENodes.end()) continue;
    if (visitedENodes.size() >= MT_RHS_STATE_READ_ENODE_LIMIT) {
      info.hasUnexpandedRhsDependency = true;
      return;
    }
    visitedENodes.insert(top);
    if (top->nodePtr) {
      if (top->nodePtr->type == NODE_REG_SRC) {
        info.rhsReadStateTargetNames.insert(top->nodePtr->name);
      } else if (top->nodePtr->type == NODE_REG_DST || top->nodePtr->type == NODE_REG_RESET) {
        bool expectedCommitRead = owner && owner->type == NODE_REG_SRC &&
                                  top->nodePtr->type == NODE_REG_DST &&
                                  owner->getDst() == top->nodePtr;
        if (!expectedCommitRead) info.hasRhsNextStateObjectRead = true;
      } else if (!top->nodePtr->assignTree.empty() &&
                 expandedNodes.find(top->nodePtr) == expandedNodes.end()) {
        if (expandedNodes.size() >= MT_RHS_STATE_READ_EXPAND_NODE_LIMIT) {
          info.hasUnexpandedRhsDependency = true;
          continue;
        }
        expandedNodes.insert(top->nodePtr);
        // Bounded expansion preserves small local proofs without recursively
        // exploding through large XiangShan combinational cones.
        for (ExpTree* tree : top->nodePtr->assignTree) {
          collectMtRhsStateReads(tree->getRoot(), info, owner, visitedENodes, expandedNodes);
        }
      }
    }
    for (ENode* child : top->child) {
      if (child) stack.push(child);
    }
  }
}

static bool nodeHasMemoryWrite(Node* node) {
  return node->type == NODE_WRITER || node->type == NODE_READWRITER;
}

static bool nodeHasMemoryRead(Node* node) {
  return node->type == NODE_READER || node->type == NODE_READWRITER;
}

static void addSerialReason(std::vector<std::string>& reasons, const std::string& reason) {
  if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
    reasons.push_back(reason);
  }
}

static void visitMtENode(ENode* root, MtBoundaryInfo& info, int& cost) {
  if (!root) return;
  std::stack<ENode*> stack;
  stack.push(root);
  while (!stack.empty()) {
    ENode* top = stack.top();
    stack.pop();
    if (!top) continue;

    if (top->nodePtr) {
      if (top->nodePtr->isArray()) info.hasArrayOrDynamicIndex = true;
      if (!isKnownNodeType(top->nodePtr->type) || top->nodePtr->type == NODE_INVALID || top->nodePtr->type == NODE_INFER) {
        info.hasUnknownNode = true;
      }
    } else {
      switch (top->opType) {
        case OP_EMPTY:
        case OP_INT:
        case OP_INDEX_INT:
          break;
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_REM:
        case OP_LT:
        case OP_LEQ:
        case OP_GT:
        case OP_GEQ:
        case OP_EQ:
        case OP_NEQ:
        case OP_DSHL:
        case OP_DSHR:
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_CAT:
        case OP_ASUINT:
        case OP_ASSINT:
        case OP_ASCLOCK:
        case OP_ASASYNCRESET:
        case OP_CVT:
        case OP_NEG:
        case OP_NOT:
        case OP_ANDR:
        case OP_ORR:
        case OP_XORR:
        case OP_PAD:
        case OP_SHL:
        case OP_SHR:
        case OP_HEAD:
        case OP_TAIL:
        case OP_BITS:
        case OP_BITS_NOSHIFT:
        case OP_MUX:
        case OP_WHEN:
        case OP_GROUP:
        case OP_SEXT:
        case OP_STMT_SEQ:
        case OP_STMT_WHEN:
        case OP_STMT_NODE:
          cost ++;
          break;
        case OP_INDEX:
          info.hasArrayOrDynamicIndex = true;
          cost ++;
          break;
        case OP_READ_MEM:
          info.hasMemoryRead = true;
          cost ++;
          break;
        case OP_WRITE_MEM:
        case OP_INFER_MEM:
          info.hasMemoryWrite = true;
          cost ++;
          break;
        case OP_RESET:
          info.hasReset = true;
          cost ++;
          break;
        case OP_PRINTF:
        case OP_ASSERT:
        case OP_EXIT:
          info.hasSpecial = true;
          cost ++;
          break;
        case OP_EXT_FUNC:
          info.hasExternal = true;
          cost ++;
          break;
        case OP_INVALID:
        default:
          info.hasUnknownOp = true;
          break;
      }
    }

    for (ENode* child : top->child) {
      if (child) stack.push(child);
    }
  }
}

static MtBoundaryInfo collectMtBoundaryInfo(SuperNode* super, int& candidateCost) {
  MtBoundaryInfo info;
  candidateCost = 0;
  info.hasStateUpdate = super->superType == SUPER_UPDATE_REG;
  info.hasReset = super->superType == SUPER_ASYNC_RESET || super->superType == SUPER_UINT_RESET;
  info.hasAsyncReset = super->superType == SUPER_ASYNC_RESET;
  info.hasExternal = super->superType == SUPER_EXTMOD;

  for (Node* member : super->member) {
    info.nodeKinds[nodeTypeName(member->type)] ++;
    info.hasStateUpdate = info.hasStateUpdate || nodeHasStateUpdate(member);
    collectStateTargetName(member, info);
    info.hasMemoryWrite = info.hasMemoryWrite || nodeHasMemoryWrite(member);
    info.hasMemoryRead = info.hasMemoryRead || nodeHasMemoryRead(member);
    info.hasReset = info.hasReset || member->isReset() || member->type == NODE_REG_RESET;
    info.hasAsyncReset = info.hasAsyncReset || member->isAsyncReset() || member->reset == ASYRESET;
    info.hasExternal = info.hasExternal || member->isExt();
    info.hasSpecial = info.hasSpecial || member->type == NODE_SPECIAL;
    info.hasArrayOrDynamicIndex = info.hasArrayOrDynamicIndex || member->isArray();
    if (!isKnownNodeType(member->type) || member->type == NODE_INVALID || member->type == NODE_INFER || member->type == NODE_MEMORY) {
      info.hasUnknownNode = true;
    }
    if (member->clock) info.clockNames.insert(member->clock->name);
    for (ExpTree* tree : member->assignTree) {
      visitMtENode(tree->getRoot(), info, candidateCost);
      visitMtENode(tree->getlval(), info, candidateCost);
      if (nodeHasStateUpdate(member)) {
        std::set<ENode*> visitedENodes;
        std::set<Node*> expandedNodes;
        collectMtRhsStateReads(tree->getRoot(), info, member, visitedENodes, expandedNodes);
      }
    }
    if (member->resetTree) {
      visitMtENode(member->resetTree->getRoot(), info, candidateCost);
      visitMtENode(member->resetTree->getlval(), info, candidateCost);
    }
  }

  if (super->resetNode) {
    info.hasReset = true;
    info.hasAsyncReset = info.hasAsyncReset || super->resetNode->isAsyncReset() || super->resetNode->reset == ASYRESET;
    if (super->resetNode->clock) info.clockNames.insert(super->resetNode->clock->name);
  }

  info.hasActivateAllPath = info.hasAsyncReset;
  return info;
}

static MtTaskInfo classifyMtTask(SuperNode* super) {
  MtTaskInfo task;
  int candidateCost = 0;
  task.boundary = collectMtBoundaryInfo(super, candidateCost);

  if (super->superType != SUPER_VALID) {
    addSerialReason(task.serialReasons, "super_type_" + std::string(superTypeName(super->superType)));
  }
  if (task.boundary.hasStateUpdate) addSerialReason(task.serialReasons, "state_update");
  if (task.boundary.hasMemoryWrite) addSerialReason(task.serialReasons, "memory_write");
  if (task.boundary.hasMemoryRead) addSerialReason(task.serialReasons, "memory_read_unsupported");
  if (task.boundary.hasReset) addSerialReason(task.serialReasons, "reset");
  if (task.boundary.hasAsyncReset) addSerialReason(task.serialReasons, "async_reset");
  if (task.boundary.hasActivateAllPath) addSerialReason(task.serialReasons, "activate_all_path");
  if (task.boundary.hasExternal) addSerialReason(task.serialReasons, "external");
  if (task.boundary.hasSpecial) addSerialReason(task.serialReasons, "special");
  if (task.boundary.hasUnknownNode) addSerialReason(task.serialReasons, "unknown_node");
  if (task.boundary.hasUnknownOp) addSerialReason(task.serialReasons, "unknown_op");
  if (task.boundary.hasArrayOrDynamicIndex) addSerialReason(task.serialReasons, "array_or_dynamic_index");

  if (task.serialReasons.empty()) {
    task.taskKind = "pure_compute";
    task.hasCandidateCost = true;
    task.candidateCost = std::max(1, candidateCost);
  } else {
    task.taskKind = "serial";
  }
  return task;
}

static std::map<int, MtTaskInfo> buildMtTaskInfoMap() {
  std::map<int, MtTaskInfo> tasks;
  for (int cppId = 0; cppId < superId; cppId ++) {
    tasks[cppId] = classifyMtTask(cppId2Super[cppId]);
  }

  for (auto& iter : tasks) {
    int cppId = iter.first;
    MtTaskInfo& task = iter.second;
    if (task.taskKind != "pure_compute") continue;

    SuperNode* super = cppId2Super[cppId];
    bool hasPred = false;
    bool hasSucc = false;
    auto checkPred = [&](SuperNode* pred) {
      if (!pred || pred->cppId < 0) return;
      hasPred = true;
      if (tasks[pred->cppId].taskKind == "serial") task.isSource = true;
    };
    auto checkSucc = [&](SuperNode* succ) {
      if (!succ || succ->cppId < 0) return;
      hasSucc = true;
      if (tasks[succ->cppId].taskKind == "serial") task.isSink = true;
    };
    for (SuperNode* pred : super->prev) checkPred(pred);
    for (SuperNode* pred : super->depPrev) checkPred(pred);
    for (SuperNode* succ : super->next) checkSucc(succ);
    for (SuperNode* succ : super->depNext) checkSucc(succ);
    for (Node* member : super->member) {
      for (int activeId : member->nextNeedActivate) {
        if (activeId < 0) continue;
        hasSucc = true;
        if (tasks[activeId].taskKind == "serial") task.isSink = true;
      }
    }
    if (!hasPred) task.isSource = true;
    if (!hasSucc) task.isSink = true;
  }
  return tasks;
}

static bool mtTasksHaveEdge(SuperNode* lhs, SuperNode* rhs) {
  int lhsId = lhs->cppId;
  int rhsId = rhs->cppId;
  if (hasCppId(lhs->prev, rhsId) || hasCppId(lhs->next, rhsId) ||
      hasCppId(lhs->depPrev, rhsId) || hasCppId(lhs->depNext, rhsId) ||
      hasCppId(rhs->prev, lhsId) || hasCppId(rhs->next, lhsId) ||
      hasCppId(rhs->depPrev, lhsId) || hasCppId(rhs->depNext, lhsId)) {
    return true;
  }
  for (Node* member : lhs->member) {
    if (member->nextActiveId.find(rhsId) != member->nextActiveId.end()) return true;
  }
  for (Node* member : rhs->member) {
    if (member->nextActiveId.find(lhsId) != member->nextActiveId.end()) return true;
  }
  return false;
}

static bool mtTasksHaveDirectedEdge(SuperNode* from, SuperNode* to) {
  int toId = to->cppId;
  int fromId = from->cppId;
  if (hasCppId(from->next, toId) || hasCppId(from->depNext, toId) ||
      hasCppId(to->prev, fromId) || hasCppId(to->depPrev, fromId)) {
    return true;
  }
  for (Node* member : from->member) {
    if (member->nextActiveId.find(toId) != member->nextActiveId.end()) return true;
  }
  return false;
}

static bool mtTaskCanEnterPureBatch(const std::map<int, MtTaskInfo>& tasks, int cppId) {
  auto iter = tasks.find(cppId);
  if (iter == tasks.end()) return false;
  if (iter->second.taskKind != "pure_compute") return false;
  if (isAlwaysActive(cppId)) return false;
  return true;
}

static bool mtCanCutEdge(const std::map<int, MtTaskInfo>& tasks, int fromCppId, int toCppId) {
  if (fromCppId == toCppId) return false;
  auto from = tasks.find(fromCppId);
  auto to = tasks.find(toCppId);
  if (from == tasks.end() || to == tasks.end()) return false;
  if (from->second.taskKind != "pure_compute" || to->second.taskKind != "pure_compute") return false;
  if (!from->second.serialReasons.empty() || !to->second.serialReasons.empty()) return false;
  if (!to->second.repcutSelected) return false;
  if (isAlwaysActive(fromCppId) || isAlwaysActive(toCppId)) return false;
  if (fromCppId / ACTIVE_WIDTH != toCppId / ACTIVE_WIDTH) return false;
  return mtTasksHaveDirectedEdge(cppId2Super[fromCppId], cppId2Super[toCppId]);
}

static bool mtTaskHasSameActiveWordHazard(const std::map<int, MtTaskInfo>& tasks, int cppId, bool allowCuts) {
  if (!mtTaskCanEnterPureBatch(tasks, cppId)) return false;
  int wordBegin = (cppId / ACTIVE_WIDTH) * ACTIVE_WIDTH;
  int wordEnd = std::min(superId, wordBegin + ACTIVE_WIDTH);
  for (int otherCppId = wordBegin; otherCppId < wordEnd; otherCppId ++) {
    if (otherCppId == cppId) continue;
    if (!mtTaskCanEnterPureBatch(tasks, otherCppId)) continue;
    if (!mtTasksHaveEdge(cppId2Super[cppId], cppId2Super[otherCppId])) continue;
    bool cutForward = allowCuts && mtCanCutEdge(tasks, cppId, otherCppId);
    bool cutBackward = allowCuts && mtCanCutEdge(tasks, otherCppId, cppId);
    if (!cutForward && !cutBackward) return true;
  }
  return false;
}

static bool mtTaskCanJoinPureBatchWithCuts(const std::map<int, MtTaskInfo>& tasks,
                                           const std::vector<int>& batch,
                                           int cppId,
                                           bool allowCuts,
                                           std::vector<MtRepCutEdge>* cutEdges) {
  if (!mtTaskCanEnterPureBatch(tasks, cppId)) return false;
  for (int existingCppId : batch) {
    if (!mtTasksHaveEdge(cppId2Super[existingCppId], cppId2Super[cppId])) continue;
    bool cutForward = allowCuts && mtCanCutEdge(tasks, existingCppId, cppId);
    bool cutBackward = allowCuts && mtCanCutEdge(tasks, cppId, existingCppId);
    if (!cutForward && !cutBackward) return false;
    if (cutEdges) {
      if (cutForward) cutEdges->push_back({existingCppId, cppId, "pure_successor_selected"});
      if (cutBackward) cutEdges->push_back({cppId, existingCppId, "pure_successor_selected"});
    }
  }
  return true;
}

static int mtTaskEstimatedCost(const std::map<int, MtTaskInfo>& tasks, int cppId) {
  auto iter = tasks.find(cppId);
  if (iter == tasks.end()) return 1;
  if (iter->second.hasCandidateCost && iter->second.candidateCost > 0) return iter->second.candidateCost;
  return 1;
}

static int mtBatchEstimatedCost(const std::map<int, MtTaskInfo>& tasks, int beginCppId, int endCppId) {
  int cost = 0;
  for (int cppId = beginCppId; cppId < endCppId; cppId ++) {
    cost += mtTaskEstimatedCost(tasks, cppId);
  }
  return cost;
}

static int mtBatchMemberNodeCost(int beginCppId, int endCppId) {
  int cost = 0;
  for (int cppId = beginCppId; cppId < endCppId; cppId ++) {
    auto iter = cppId2Super.find(cppId);
    if (iter != cppId2Super.end() && iter->second) cost += static_cast<int>(iter->second->member.size());
  }
  return cost;
}

static bool mtActiveWordIsWhole(int beginCppId) {
  for (int j = 0; j < ACTIVE_WIDTH && beginCppId + j < superId; j ++) {
    if (isAlwaysActive(beginCppId + j)) return false;
  }
  return true;
}

static int mtPureBatchShardCount() {
  return (superId + MT_PURE_BATCH_SHARD_SIZE - 1) / MT_PURE_BATCH_SHARD_SIZE;
}

static void mtAddCoarseBlocker(MtCoarseRegion& region, const std::string& blocker) {
  if (std::find(region.blockers.begin(), region.blockers.end(), blocker) == region.blockers.end()) {
    region.blockers.push_back(blocker);
  }
}

static bool mtTaskHasActiveEdgeTo(int fromCppId, int toCppId) {
  auto iter = cppId2Super.find(fromCppId);
  if (iter == cppId2Super.end() || !iter->second) return false;
  for (Node* member : iter->second->member) {
    if (member && member->nextActiveId.find(toCppId) != member->nextActiveId.end()) return true;
  }
  return false;
}

static bool mtTaskHasDependencyEdgeTo(int fromCppId, int toCppId) {
  auto from = cppId2Super.find(fromCppId);
  auto to = cppId2Super.find(toCppId);
  if (from == cppId2Super.end() || to == cppId2Super.end() || !from->second || !to->second) return false;
  if (hasCppId(from->second->next, toCppId) || hasCppId(from->second->depNext, toCppId) ||
      hasCppId(to->second->prev, fromCppId) || hasCppId(to->second->depPrev, fromCppId)) {
    return true;
  }
  return false;
}

static bool mtTaskHasOrderingEdgeTo(int fromCppId, int toCppId) {
  return mtTaskHasDependencyEdgeTo(fromCppId, toCppId) || mtTaskHasActiveEdgeTo(fromCppId, toCppId);
}

static void mtAddCoarseLayers(MtCoarseRegion& region) {
  int n = region.endCppId - region.beginCppId;
  if (n <= 0) return;
  std::vector<int> indegree(n, 0);
  std::vector<std::vector<int>> edges(n);
  for (int from = region.beginCppId; from < region.endCppId; from ++) {
    for (int to = region.beginCppId; to < region.endCppId; to ++) {
      if (from == to) continue;
      if (!mtTaskHasOrderingEdgeTo(from, to)) continue;
      int fromIndex = from - region.beginCppId;
      int toIndex = to - region.beginCppId;
      edges[fromIndex].push_back(toIndex);
      indegree[toIndex] ++;
    }
  }

  std::vector<bool> emitted(n, false);
  int emittedCount = 0;
  while (emittedCount < n) {
    MtCoarseLayer layer;
    for (int i = 0; i < n; i ++) {
      if (!emitted[i] && indegree[i] == 0) layer.taskCppIds.push_back(region.beginCppId + i);
    }
    if (layer.taskCppIds.empty()) {
      mtAddCoarseBlocker(region, "data_dependency");
      mtAddCoarseBlocker(region, "codegen_runtime_limit");
      region.layers.clear();
      region.runtimeEligible = false;
      region.estimatedLayerCount = 0;
      region.estimatedMaxParallelWidth = 0;
      return;
    }
    for (int cppId : layer.taskCppIds) {
      int index = cppId - region.beginCppId;
      if (emitted[index]) continue;
      emitted[index] = true;
      emittedCount ++;
      for (int toIndex : edges[index]) indegree[toIndex] --;
    }
    region.estimatedMaxParallelWidth = std::max(region.estimatedMaxParallelWidth, static_cast<int>(layer.taskCppIds.size()));
    region.layers.push_back(layer);
  }
  region.estimatedLayerCount = static_cast<int>(region.layers.size());
}

static void mtAddCoarseMTasks(MtCoarseRegion& region) {
  region.mtasks.clear();
  int n = region.endCppId - region.beginCppId;
  if (n <= 0 || region.layers.empty()) return;

  std::vector<int> parent(n);
  for (int i = 0; i < n; i ++) parent[i] = i;
  auto findRoot = [&](int value) {
    int root = value;
    while (parent[root] != root) root = parent[root];
    while (parent[value] != value) {
      int next = parent[value];
      parent[value] = root;
      value = next;
    }
    return root;
  };
  auto unite = [&](int a, int b) {
    int rootA = findRoot(a);
    int rootB = findRoot(b);
    if (rootA != rootB) parent[rootB] = rootA;
  };

  for (int from = region.beginCppId; from < region.endCppId; from ++) {
    for (int to = region.beginCppId; to < region.endCppId; to ++) {
      if (from == to) continue;
      if (!mtTaskHasOrderingEdgeTo(from, to)) continue;
      unite(from - region.beginCppId, to - region.beginCppId);
    }
  }

  std::map<int, int> groupIndexByRoot;
  std::map<int, int> layerIndexByCppId;
  for (size_t layerIdx = 0; layerIdx < region.layers.size(); layerIdx ++) {
    for (int cppId : region.layers[layerIdx].taskCppIds) {
      int local = cppId - region.beginCppId;
      int root = findRoot(local);
      if (groupIndexByRoot.find(root) == groupIndexByRoot.end()) {
        int groupIndex = static_cast<int>(region.mtasks.size());
        groupIndexByRoot[root] = groupIndex;
        region.mtasks.push_back(MtCoarseMTask());
      }
      int groupIndex = groupIndexByRoot[root];
      while (region.mtasks[groupIndex].layerTaskCppIds.size() <= layerIdx) {
        region.mtasks[groupIndex].layerTaskCppIds.push_back(std::vector<int>());
      }
      region.mtasks[groupIndex].layerTaskCppIds[layerIdx].push_back(cppId);
      region.mtasks[groupIndex].taskCount ++;
      layerIndexByCppId[cppId] = static_cast<int>(layerIdx);
    }
  }

  for (int from = region.beginCppId; from < region.endCppId; from ++) {
    for (int to = region.beginCppId; to < region.endCppId; to ++) {
      if (from == to) continue;
      if (!mtTaskHasOrderingEdgeTo(from, to)) continue;
      int fromRoot = findRoot(from - region.beginCppId);
      int toRoot = findRoot(to - region.beginCppId);
      if (fromRoot != toRoot) continue;
      int groupIndex = groupIndexByRoot[fromRoot];
      region.mtasks[groupIndex].orderingEdgeCount ++;
    }
  }
}

static MtCoarseRegion mtBuildCoarseRegion(const std::map<int, MtTaskInfo>& tasks, int beginCppId, int endCppId) {
  MtCoarseRegion region;
  region.beginCppId = beginCppId;
  region.endCppId = endCppId;
  region.beginActiveWord = beginCppId / ACTIVE_WIDTH;
  region.endActiveWord = (endCppId - 1) / ACTIVE_WIDTH + 1;
  region.taskCount = endCppId - beginCppId;
  region.activeWordSpan = region.endActiveWord - region.beginActiveWord;
  region.staticCost = mtBatchEstimatedCost(tasks, beginCppId, endCppId);
  region.memberNodeCost = mtBatchMemberNodeCost(beginCppId, endCppId);
  region.expectedActiveCost = region.staticCost;
  region.pureTaskCount = region.taskCount;

  for (int cppId = beginCppId; cppId < endCppId; cppId ++) {
    auto iter = tasks.find(cppId);
    if (iter == tasks.end() || iter->second.taskKind != "pure_compute") {
      region.pureTaskCount --;
      region.serialBlockerCount ++;
      mtAddCoarseBlocker(region, "serial_task");
      continue;
    }
    if (isAlwaysActive(cppId)) mtAddCoarseBlocker(region, "codegen_runtime_limit");
  }

  for (int from = beginCppId; from < endCppId; from ++) {
    for (int to = beginCppId; to < endCppId; to ++) {
      if (from == to) continue;
      if (mtTaskHasDependencyEdgeTo(from, to)) {
        region.dependencyEdgeCount ++;
        if (from > to) mtAddCoarseBlocker(region, "data_dependency");
      }
      if (mtTaskHasActiveEdgeTo(from, to)) {
        region.activeVisibilityEdgeCount ++;
        if (to > from) region.sameCycleActivationHazardCount ++;
        if (from > to) mtAddCoarseBlocker(region, "active_visibility_edge");
      }
      if (mtCanCutEdge(tasks, from, to)) {
        region.replicationCandidateCount ++;
        region.repcutLiteCouldHelp = true;
      }
    }
  }

  if (beginCppId % ACTIVE_WIDTH != 0 || endCppId % ACTIVE_WIDTH != 0) {
    mtAddCoarseBlocker(region, "codegen_runtime_limit");
  }
  if (region.taskCount < globalConfig.MtActiveFrequencyCostThreshold) {
    mtAddCoarseBlocker(region, "codegen_runtime_limit");
  }
  if (region.activeWordSpan <= 1) {
    mtAddCoarseBlocker(region, "codegen_runtime_limit");
  }
  if (region.pureTaskCount != region.taskCount) {
    mtAddCoarseBlocker(region, "serial_task");
  }

  region.runtimeEligible = region.blockers.empty();
  mtAddCoarseLayers(region);
  if (region.layers.empty()) {
    region.runtimeEligible = false;
  }
  if (region.estimatedMaxParallelWidth < 2) {
    mtAddCoarseBlocker(region, "codegen_runtime_limit");
    region.runtimeEligible = false;
  }
  if (region.runtimeEligible) mtAddCoarseMTasks(region);
  return region;
}

static MtCoarseRegionPlan planMtCoarseRegions(const std::map<int, MtTaskInfo>& tasks) {
  MtCoarseRegionPlan plan;
  for (int beginWord = 0; beginWord * ACTIVE_WIDTH < superId; beginWord ++) {
    int beginCppId = beginWord * ACTIVE_WIDTH;
    if (!mtActiveWordIsWhole(beginCppId)) continue;
    int endWord = beginWord;
    while (endWord * ACTIVE_WIDTH < superId) {
      int wordBegin = endWord * ACTIVE_WIDTH;
      if (!mtActiveWordIsWhole(wordBegin)) break;
      int wordEnd = std::min(superId, wordBegin + ACTIVE_WIDTH);
      bool wholeWordPure = wordEnd - wordBegin == ACTIVE_WIDTH;
      for (int cppId = wordBegin; cppId < wordEnd; cppId ++) {
        if (!mtTaskCanEnterPureBatch(tasks, cppId)) {
          wholeWordPure = false;
          break;
        }
      }
      if (!wholeWordPure) break;
      endWord ++;
    }
    int endCppId = endWord * ACTIVE_WIDTH;
    if (endCppId - beginCppId >= ACTIVE_WIDTH) {
      MtCoarseRegion region = mtBuildCoarseRegion(tasks, beginCppId, endCppId);
      plan.regions.push_back(region);
      beginWord = std::max(beginWord, endWord - 1);
    }
  }
  return plan;
}

static int mtHistBucket(int count) {
  if (count <= 1) return 0;
  if (count == 2) return 1;
  if (count <= 4) return 2;
  if (count <= 8) return 3;
  if (count <= 15) return 4;
  return 5;
}

static MtCoarseProfileFacts mtComputeCoarseProfileFacts(const MtCoarseRegionPlan& coarsePlan) {
  MtCoarseProfileFacts facts;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (!region.runtimeEligible) continue;
    facts.runtimeEligibleRegionCount ++;
    facts.runtimeLayerCount += static_cast<int>(region.layers.size());
    facts.runtimeMTaskCount += static_cast<int>(region.mtasks.size());
    facts.maxRegionLayerCount = std::max(facts.maxRegionLayerCount, static_cast<int>(region.layers.size()));
    facts.regionLayerCountHist[mtHistBucket(static_cast<int>(region.layers.size()))] ++;
    for (const MtCoarseLayer& layer : region.layers) {
      facts.layerSizeHist[mtHistBucket(static_cast<int>(layer.taskCppIds.size()))] ++;
    }
  }
  return facts;
}

static MtPureBatchPlan planMtPureBatchesLegacy(const std::map<int, MtTaskInfo>& tasks, bool allowCuts) {
  MtPureBatchPlan plan;
  for (int idx = 0; idx < superId; idx ++) {
    int id;
    uint64_t mask;
    std::tie(id, mask) = setIdxMask(idx);
    bool activeWhole = mtActiveWordIsWhole(idx);
    if (!activeWhole || !mtTaskCanEnterPureBatch(tasks, idx)) continue;
    plan.segmentCount ++;

    std::vector<int> batch;
    std::vector<MtRepCutEdge> batchCutEdges;
    batch.push_back(idx);
    int batchEnd = idx + 1;
    while (batchEnd < superId && batchEnd / ACTIVE_WIDTH == id &&
           mtTaskCanJoinPureBatchWithCuts(tasks, batch, batchEnd, allowCuts, &batchCutEdges)) {
      batch.push_back(batchEnd);
      batchEnd ++;
    }
    if (batchEnd - idx > 1) {
      plan.batches.push_back(std::make_pair(idx, batchEnd));
      plan.cutEdges.insert(plan.cutEdges.end(), batchCutEdges.begin(), batchCutEdges.end());
      idx = batchEnd - 1;
    }
  }
  return plan;
}

static MtPureBatchPlan planMtPureBatchesActiveFrequency(const std::map<int, MtTaskInfo>& tasks, bool allowCuts) {
  MtPureBatchPlan plan;
  int threshold = globalConfig.MtActiveFrequencyCostThreshold;
  for (int wordBegin = 0; wordBegin < superId; wordBegin += ACTIVE_WIDTH) {
    if (!mtActiveWordIsWhole(wordBegin)) continue;
    int wordEnd = std::min(superId, wordBegin + ACTIVE_WIDTH);
    int idx = wordBegin;
    while (idx < wordEnd) {
      while (idx < wordEnd && !mtTaskCanEnterPureBatch(tasks, idx)) idx ++;
      if (idx >= wordEnd) break;
      plan.segmentCount ++;

      std::vector<int> batch;
      std::vector<MtRepCutEdge> batchCutEdges;
      int batchBegin = idx;
      int batchEnd = idx;
      while (batchEnd < wordEnd &&
             mtTaskCanJoinPureBatchWithCuts(tasks, batch, batchEnd, allowCuts, &batchCutEdges)) {
        batch.push_back(batchEnd);
        batchEnd ++;
      }
      if (batchEnd - batchBegin > 1 &&
          mtBatchEstimatedCost(tasks, batchBegin, batchEnd) >= threshold) {
        plan.batches.push_back(std::make_pair(batchBegin, batchEnd));
        plan.cutEdges.insert(plan.cutEdges.end(), batchCutEdges.begin(), batchCutEdges.end());
      }
      idx = std::max(batchEnd, batchBegin + 1);
    }
  }
  return plan;
}

static MtPureBatchPlan planMtPureBatches(const std::map<int, MtTaskInfo>& tasks, bool allowCuts) {
  if (globalConfig.MtBatchFormationMode == "active-frequency" ||
      globalConfig.MtBatchFormationMode == "coarse") {
    return planMtPureBatchesActiveFrequency(tasks, allowCuts);
  }
  return planMtPureBatchesLegacy(tasks, allowCuts);
}

static std::string mtRepCutIntLiteral(ENode* enode) {
  if (!enode->strVal.empty()) {
    std::string value = enode->strVal;
    if (!value.empty() && value[0] == '-') return "(" + value + ")";
    return value;
  }
  if (!enode->values.empty()) return "0x" + std::to_string(enode->values[0]);
  return "";
}

static bool mtRepCutExprString(ENode* enode,
                               std::string& expr,
                               std::string& reason,
                               const std::map<Node*, std::string>& replacements = {}) {
  if (!enode) {
    reason = "unsupported_expr";
    return false;
  }
  if (enode->nodePtr) {
    auto repl = replacements.find(enode->nodePtr);
    expr = repl == replacements.end() ? enode->nodePtr->name : repl->second;
    return true;
  }
  if (enode->opType == OP_INT) {
    expr = mtRepCutIntLiteral(enode);
    if (expr.empty()) reason = "unsupported_expr";
    return !expr.empty();
  }

  auto childExpr = [&](size_t idx, std::string& out) {
    if (idx >= enode->getChildNum()) {
      reason = "unsupported_expr";
      return false;
    }
    return mtRepCutExprString(enode->getChild(idx), out, reason, replacements);
  };

  std::string lhs;
  std::string rhs;
  switch (enode->opType) {
    case OP_ADD:
      if (!childExpr(0, lhs) || !childExpr(1, rhs)) return false;
      expr = "(" + lhs + " + " + rhs + ")";
      if (enode->width > 0) expr = "(" + expr + " & " + bitMask(enode->width) + ")";
      return true;
    case OP_AND:
      if (!childExpr(0, lhs) || !childExpr(1, rhs)) return false;
      expr = "(" + lhs + " & " + rhs + ")";
      return true;
    case OP_OR:
      if (!childExpr(0, lhs) || !childExpr(1, rhs)) return false;
      expr = "(" + lhs + " | " + rhs + ")";
      return true;
    case OP_XOR:
      if (!childExpr(0, lhs) || !childExpr(1, rhs)) return false;
      expr = "(" + lhs + " ^ " + rhs + ")";
      return true;
    case OP_TAIL:
      if (!childExpr(0, lhs)) return false;
      if (enode->values.empty()) {
        reason = "unsupported_expr";
        return false;
      }
      expr = "(" + lhs + " & " + bitMask(MIN(enode->width, enode->values[0])) + ")";
      return true;
    case OP_BITS:
    case OP_BITS_NOSHIFT:
      if (!childExpr(0, lhs)) return false;
      if (enode->values.size() < 2) {
        reason = "unsupported_expr";
        return false;
      }
      expr = "((" + lhs + " >> " + std::to_string(enode->values[1]) + ") & " + bitMask(enode->width) + ")";
      return true;
    case OP_PAD:
    case OP_ASUINT:
      if (!childExpr(0, lhs)) return false;
      expr = lhs;
      return true;
    case OP_MUX: {
      std::string cond;
      std::string tval;
      std::string fval;
      if (!childExpr(0, cond) || !childExpr(1, tval) || !childExpr(2, fval)) return false;
      if (enode->width == 1) expr = "((" + cond + " & " + tval + ") | ((!" + cond + ") & " + fval + "))";
      else expr = format("((-(%s)%s & %s) | ((-(%s)!%s) & %s))",
                         widthUType(enode->width).c_str(), cond.c_str(), tval.c_str(),
                         widthUType(enode->width).c_str(), cond.c_str(), fval.c_str());
      return true;
    }
    default:
      reason = "unsupported_expr";
      return false;
  }
}

static Node* mtRepCutSingleProducedNode(int cppId, std::string& reason) {
  auto superIter = cppId2Super.find(cppId);
  if (superIter == cppId2Super.end()) {
    reason = "missing_clone";
    return nullptr;
  }
  std::vector<Node*> candidates;
  for (Node* member : superIter->second->member) {
    if (!member || member->isLocal() || member->isArray()) continue;
    if (member->type != NODE_OTHERS) continue;
    if (member->assignTree.size() != 1) continue;
    candidates.push_back(member);
  }
  if (candidates.size() != 1) {
    reason = "missing_clone";
    return nullptr;
  }
  return candidates[0];
}

static bool mtRepCutNodeOnlyFeedsSink(Node* node, int sinkCppId) {
  if (!node) return false;
  bool hasSink = false;
  for (Node* next : node->next) {
    if (!next || !next->super || next->super->cppId < 0) continue;
    if (next->super->cppId != sinkCppId) return false;
    hasSink = true;
  }
  for (Node* next : node->depNext) {
    if (!next || !next->super || next->super->cppId < 0) continue;
    if (next->super->cppId != sinkCppId) return false;
    hasSink = true;
  }
  return hasSink;
}

static std::map<Node*, std::string> mtRepCutReplacementMap(const std::vector<MtRepCutClone>& clones) {
  std::map<Node*, std::string> replacements;
  for (const MtRepCutClone& clone : clones) {
    if (clone.sourceNode && clone.fallbackReason.empty()) replacements[clone.sourceNode] = clone.cloneName;
  }
  return replacements;
}

static MtRepCutClone mtPlanRepCutCloneForEdge(const MtRepCutEdge& edge) {
  MtRepCutClone clone;
  clone.sourceCppId = edge.fromCppId;
  clone.sinkCppId = edge.toCppId;
  std::string reason;
  Node* sourceNode = mtRepCutSingleProducedNode(edge.fromCppId, reason);
  if (!sourceNode) {
    clone.fallbackReason = reason.empty() ? "missing_clone" : reason;
    return clone;
  }
  if (!mtRepCutNodeOnlyFeedsSink(sourceNode, edge.toCppId)) {
    clone.sourceNode = sourceNode;
    clone.fallbackReason = "multi_consumer_not_supported";
    return clone;
  }
  if (sourceNode->assignTree.size() != 1) {
    clone.sourceNode = sourceNode;
    clone.fallbackReason = "missing_clone";
    return clone;
  }
  std::string expr;
  if (!mtRepCutExprString(sourceNode->assignTree[0]->getRoot(), expr, reason)) {
    clone.sourceNode = sourceNode;
    clone.fallbackReason = reason.empty() ? "unsupported_expr" : reason;
    return clone;
  }
  clone.sourceNode = sourceNode;
  clone.cloneName = format("repcut_%d_%d_%s", edge.fromCppId, edge.toCppId, sourceNode->name.c_str());
  clone.expr = expr;
  return clone;
}

static bool mtRepCutEdgeInBatch(const MtRepCutEdge& edge, const std::pair<int, int>& batch) {
  return batch.first <= edge.fromCppId && edge.fromCppId < batch.second &&
         batch.first <= edge.toCppId && edge.toCppId < batch.second;
}

static bool mtRepCutHasClonedEdge(const std::set<std::pair<int, int>>& clonedEdges,
                                  int fromCppId,
                                  int toCppId) {
  return clonedEdges.find(std::make_pair(fromCppId, toCppId)) != clonedEdges.end();
}

static MtRepCutSemanticPlan planMtRepCutSemantics(const std::map<int, MtTaskInfo>& tasks) {
  MtRepCutSemanticPlan semanticPlan;
  semanticPlan.batchPlan = planMtPureBatches(tasks, globalConfig.MtRepCutLiteMode == "on");

  for (auto batch : semanticPlan.batchPlan.batches) {
    MtRepCutBatch cutBatch;
    cutBatch.beginCppId = batch.first;
    cutBatch.endCppId = batch.second;
    std::vector<MtRepCutClone> batchClones;
    std::set<std::pair<int, int>> clonedEdges;
    for (const MtRepCutEdge& edge : semanticPlan.batchPlan.cutEdges) {
      if (!mtRepCutEdgeInBatch(edge, batch)) continue;
      cutBatch.cutEdgeCount ++;
      cutBatch.forcedSinkCppIds.insert(edge.toCppId);
      MtRepCutClone clone = mtPlanRepCutCloneForEdge(edge);
      if (!clone.fallbackReason.empty() && cutBatch.fallbackReason.empty()) {
        cutBatch.fallbackReason = clone.fallbackReason;
      } else if (clone.fallbackReason.empty()) {
        cutBatch.cloneCount ++;
        clonedEdges.insert(std::make_pair(clone.sourceCppId, clone.sinkCppId));
      }
      batchClones.push_back(clone);
    }
    if (cutBatch.cutEdgeCount == 0) continue;
    bool forcedSinkInputsCloned = cutBatch.fallbackReason.empty() &&
                                  cutBatch.cloneCount == cutBatch.cutEdgeCount &&
                                  !cutBatch.forcedSinkCppIds.empty();
    for (int sinkCppId : cutBatch.forcedSinkCppIds) {
      auto sink = tasks.find(sinkCppId);
      if (sink == tasks.end() || sink->second.taskKind != "pure_compute" ||
          !sink->second.repcutSelected || !sink->second.serialReasons.empty()) {
        forcedSinkInputsCloned = false;
        if (cutBatch.fallbackReason.empty()) cutBatch.fallbackReason = "unsafe_forced_sink";
        break;
      }
      for (int sourceCppId = batch.first; sourceCppId < batch.second; sourceCppId ++) {
        if (sourceCppId == sinkCppId) continue;
        if (!mtTasksHaveDirectedEdge(cppId2Super[sourceCppId], cppId2Super[sinkCppId])) continue;
        if (!mtRepCutHasClonedEdge(clonedEdges, sourceCppId, sinkCppId)) {
          forcedSinkInputsCloned = false;
          if (cutBatch.fallbackReason.empty()) cutBatch.fallbackReason = "forced_sink_input_without_clone";
          break;
        }
      }
      if (!forcedSinkInputsCloned) break;
      cutBatch.forcedSinkMask |= (uint64_t)1 << (sinkCppId % ACTIVE_WIDTH);
    }
    cutBatch.parallelSafe = forcedSinkInputsCloned && cutBatch.forcedSinkMask != 0;
    cutBatch.forcedSerial = !cutBatch.parallelSafe;
    if (cutBatch.forcedSerial && cutBatch.fallbackReason.empty()) cutBatch.fallbackReason = "missing_clone";
    if (cutBatch.parallelSafe) {
      cutBatch.forcedSinkActivation = true;
      cutBatch.parallelSafeReason = "all_forced_sink_cut_inputs_cloned";
    }
    semanticPlan.cutBatches.push_back(cutBatch);
    semanticPlan.clones.insert(semanticPlan.clones.end(), batchClones.begin(), batchClones.end());
  }

  return semanticPlan;
}

static std::vector<MtRepCutClone> mtRepCutClonesForSink(const MtRepCutSemanticPlan& semanticPlan, int sinkCppId) {
  std::vector<MtRepCutClone> clones;
  for (const MtRepCutClone& clone : semanticPlan.clones) {
    if (clone.sinkCppId == sinkCppId && clone.fallbackReason.empty()) clones.push_back(clone);
  }
  return clones;
}

static bool mtRepCutNameChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
}

static std::string mtRepCutReplaceNodeNames(const std::string& text, const std::map<Node*, std::string>& replacements) {
  std::string result = text;
  for (const auto& repl : replacements) {
    const std::string& from = repl.first->name;
    const std::string& to = repl.second;
    std::string replaced;
    size_t pos = 0;
    while (pos < result.size()) {
      size_t hit = result.find(from, pos);
      if (hit == std::string::npos) {
        replaced.append(result.substr(pos));
        break;
      }
      bool leftOk = hit == 0 || !mtRepCutNameChar(result[hit - 1]);
      bool rightOk = hit + from.size() >= result.size() || !mtRepCutNameChar(result[hit + from.size()]);
      replaced.append(result.substr(pos, hit - pos));
      if (leftOk && rightOk) {
        replaced.append(to);
      } else {
        replaced.append(from);
      }
      pos = hit + from.size();
    }
    result.swap(replaced);
  }
  return result;
}

static uint64_t mtRepCutForcedSinkMaskForBatch(const MtRepCutSemanticPlan& semanticPlan, int beginCppId) {
  for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
    if (batch.beginCppId == beginCppId && batch.parallelSafe) return batch.forcedSinkMask;
  }
  return 0;
}

static void collectMtTaskRepCutCounts(std::map<int, MtTaskInfo>& tasks) {
  for (auto& iter : tasks) {
    MtTaskInfo& task = iter.second;
    task.repcutRole = "none";
    task.repcutSourceCount = 0;
    task.repcutSinkCount = 0;
    task.repcutFanout = 0;
    task.repcutCopyCost = task.hasCandidateCost ? task.candidateCost : 0;
    task.repcutBlockReason.clear();
    task.repcutSelected = false;
  }

  for (auto& iter : tasks) {
    int cppId = iter.first;
    MtTaskInfo& task = iter.second;
    SuperNode* super = cppId2Super[cppId];
    std::set<int> predCppIds;
    std::set<int> succCppIds;
    std::set<int> activeFanout;

    addCppIdsIfExecutable(predCppIds, super->prev);
    addCppIdsIfExecutable(predCppIds, super->depPrev);
    addCppIdsIfExecutable(succCppIds, super->next);
    addCppIdsIfExecutable(succCppIds, super->depNext);
    for (Node* member : super->member) {
      for (int activeId : member->nextNeedActivate) {
        if (activeId >= 0) activeFanout.insert(activeId);
      }
    }

    int sourceCount = 0;
    for (int predId : predCppIds) {
      auto pred = tasks.find(predId);
      if (pred != tasks.end() && pred->second.taskKind == "serial") sourceCount ++;
    }
    int sinkCount = 0;
    std::set<int> sinks = succCppIds;
    sinks.insert(activeFanout.begin(), activeFanout.end());
    for (int succId : sinks) {
      auto succ = tasks.find(succId);
      if (succ != tasks.end() && succ->second.taskKind == "serial") sinkCount ++;
    }

    if (task.taskKind == "pure_compute") {
      if (sourceCount == 0 && task.isSource) sourceCount = 1;
      if (sinkCount == 0 && task.isSink) sinkCount = 1;
      task.repcutSourceCount = sourceCount;
      task.repcutSinkCount = sinkCount;
      task.repcutFanout = static_cast<int>(sinks.size());
      if (sourceCount > 0 && sinkCount > 0) task.repcutRole = "candidate";
      else if (sourceCount > 0) task.repcutRole = "source";
      else if (sinkCount > 0) task.repcutRole = "sink";
    } else {
      task.repcutFanout = static_cast<int>(sinks.size());
      if (!task.serialReasons.empty()) task.repcutBlockReason = task.serialReasons.front();
      else task.repcutBlockReason = "serial_without_reason";
    }
  }
}

static std::map<int, MtTaskInfo> buildMtTaskInfoMapWithRepCut() {
  std::map<int, MtTaskInfo> tasks = buildMtTaskInfoMap();
  collectMtTaskRepCutCounts(tasks);
  return tasks;
}

static std::string repcutBlockReasonForTask(const MtTaskInfo& task) {
  if (task.taskKind != "pure_compute") {
    if (!task.serialReasons.empty()) return task.serialReasons.front();
    return task.repcutBlockReason.empty() ? "serial_without_reason" : task.repcutBlockReason;
  }
  if (!task.hasCandidateCost) return "missing_candidate_cost";
  if (task.repcutRole != "candidate") return "not_boundary_candidate";
  if (task.repcutSourceCount <= 0) return "missing_source_evidence";
  if (task.repcutSinkCount <= 0) return "missing_sink_evidence";
  if (task.repcutCopyCost <= 0) return "missing_copy_cost";
  return "";
}

static bool mtStateUpdateHasMemoryOrDynamicArray(const MtBoundaryInfo& boundary) {
  return boundary.hasMemoryWrite || boundary.hasMemoryRead || boundary.hasArrayOrDynamicIndex;
}

static bool mtStateUpdateHasExternalOrSpecial(const MtBoundaryInfo& boundary) {
  return boundary.hasExternal || boundary.hasSpecial || boundary.hasUnknownNode || boundary.hasUnknownOp;
}

static bool mtStateUpdateHasSameCycleTargetRead(const MtBoundaryInfo& boundary,
                                                const std::set<std::string>& allStateTargetNames) {
  if (boundary.hasRhsNextStateObjectRead) return true;
  for (const std::string& name : boundary.rhsReadStateTargetNames) {
    if (boundary.stateTargetNames.find(name) != boundary.stateTargetNames.end()) return true;
    if (allStateTargetNames.find(name) != allStateTargetNames.end()) return true;
  }
  return false;
}

static bool mtStateUpdateCanClassifyRhsTiming(const MtBoundaryInfo& boundary) {
  if (!boundary.hasStateUpdate) return false;
  if (boundary.hasAmbiguousStateTarget || boundary.stateTargetNames.size() != 1) return false;
  if (boundary.hasReset || boundary.hasAsyncReset || boundary.hasActivateAllPath) return false;
  if (mtStateUpdateHasMemoryOrDynamicArray(boundary)) return false;
  if (mtStateUpdateHasExternalOrSpecial(boundary)) return false;
  if (boundary.hasRhsNextStateObjectRead) return false;
  if (boundary.hasUnexpandedRhsDependency) return false;
  return true;
}

static std::string mtStateUpdateRhsTimingClass(const MtBoundaryInfo& boundary) {
  if (!mtStateUpdateCanClassifyRhsTiming(boundary)) return "unknown";
  if (boundary.stateSourceCommitCount == 1 && boundary.stateNextUpdateCount == 0 && boundary.stateResetUpdateCount == 0) {
    return "precomputed";
  }
  if (boundary.stateSourceCommitCount == 0 && boundary.stateNextUpdateCount == 1 && boundary.stateResetUpdateCount == 0) {
    return "old_state_only";
  }
  return "unknown";
}

static std::string mtStateUpdateRhsTimingEvidence(const MtBoundaryInfo& boundary,
                                                  const std::string& rhsTimingClass) {
  if (rhsTimingClass == "precomputed") return "reg_src_commit_reads_next_state_object";
  if (rhsTimingClass == "old_state_only") return "reg_dst_assign_tree_old_state_only";
  if (!boundary.hasStateUpdate) return "no_state_update";
  if (boundary.hasAmbiguousStateTarget || boundary.stateTargetNames.empty()) return "target_identity_ambiguous";
  if (boundary.stateTargetNames.size() > 1) return "multiple_state_targets";
  if (boundary.hasReset || boundary.hasAsyncReset || boundary.hasActivateAllPath) return "reset_or_activate_all";
  if (mtStateUpdateHasMemoryOrDynamicArray(boundary)) return "memory_or_dynamic_array";
  if (mtStateUpdateHasExternalOrSpecial(boundary)) return "external_or_special_or_unknown";
  if (boundary.hasRhsNextStateObjectRead) return "rhs_reads_next_state_object";
  if (boundary.hasUnexpandedRhsDependency) return "rhs_dependency_unexpanded";
  return "mixed_or_unproven_state_update";
}

static bool mtStateUpdateActivationCanUseDelta(const MtBoundaryInfo& boundary) {
  if (!boundary.hasStateUpdate) return false;
  if (boundary.hasReset || boundary.hasAsyncReset || boundary.hasActivateAllPath) return false;
  if (mtStateUpdateHasMemoryOrDynamicArray(boundary)) return false;
  if (mtStateUpdateHasExternalOrSpecial(boundary)) return false;
  return true;
}

static std::vector<std::string> mtStateUpdateBlockReasons(const MtBoundaryInfo& boundary,
                                                          SuperNode* super,
                                                          const std::string& rhsTimingClass,
                                                          bool rhsReadsSameCycleTarget) {
  std::vector<std::string> reasons;
  if (!boundary.hasStateUpdate) {
    addSerialReason(reasons, "no_state_update");
  } else {
    if (boundary.stateTargetNames.empty() || boundary.hasAmbiguousStateTarget) {
      addSerialReason(reasons, "target_identity_ambiguous");
    }
    if (boundary.stateTargetNames.size() > 1) addSerialReason(reasons, "multiple_state_targets");
    if (rhsTimingClass == "unknown") addSerialReason(reasons, "rhs_timing_unknown");
    if (rhsReadsSameCycleTarget) addSerialReason(reasons, "rhs_reads_same_cycle_target");
  }
  if (boundary.hasReset) addSerialReason(reasons, "reset_behavior");
  if (boundary.hasAsyncReset) addSerialReason(reasons, "async_reset_behavior");
  if (mtStateUpdateHasMemoryOrDynamicArray(boundary)) addSerialReason(reasons, "memory_or_dynamic_array");
  if (mtStateUpdateHasExternalOrSpecial(boundary)) addSerialReason(reasons, "external_or_special_or_unknown");
  if (boundary.hasStateUpdate && !mtStateUpdateActivationCanUseDelta(boundary)) addSerialReason(reasons, "activation_delta_unproven");
  if (super->superType != SUPER_VALID) addSerialReason(reasons, "non_valid_super_type");
  return reasons;
}

static std::string mtStateUpdateCandidateKind(const MtBoundaryInfo& boundary,
                                              const std::vector<std::string>& blockReasons) {
  if (!boundary.hasStateUpdate) return "blocked";
  if (blockReasons.empty() && boundary.stateTargetNames.size() == 1) return "safe_candidate";
  if (blockReasons.size() == 1 && blockReasons[0] == "multiple_state_targets") return "needs_split";
  return "blocked";
}

struct MtStateTargetWriterInfo {
  int writerCount = 0;
  std::set<int> writerCppIds;
  int multiTargetWriterCount = 0;
};

struct MtStateTargetWriterUniverse {
  std::map<std::string, MtStateTargetWriterInfo> targetWriters;
  bool hasIncompleteWriterUniverse = false;
  std::set<int> incompleteWriterCppIds;
};

static const size_t MT_STATE_TARGET_WRITER_ID_LIMIT = 16;

static MtStateTargetWriterUniverse
collectMtStateTargetWriters(const std::map<int, MtTaskInfo>& mtTasks) {
  MtStateTargetWriterUniverse universe;
  for (const auto& iter : mtTasks) {
    int cppId = iter.first;
    const MtBoundaryInfo& boundary = iter.second.boundary;
    if (!boundary.hasStateUpdate) continue;
    if (boundary.stateTargetNames.empty() || boundary.hasAmbiguousStateTarget) {
      universe.hasIncompleteWriterUniverse = true;
      universe.incompleteWriterCppIds.insert(cppId);
      continue;
    }
    bool multiTargetWriter = boundary.stateTargetNames.size() > 1;
    for (const std::string& target : boundary.stateTargetNames) {
      universe.targetWriters[target].writerCount ++;
      universe.targetWriters[target].writerCppIds.insert(cppId);
      if (multiTargetWriter) universe.targetWriters[target].multiTargetWriterCount ++;
    }
  }
  return universe;
}

static MtStateTargetWriterInfo mtStateUpdateWriterInfo(
    const MtBoundaryInfo& boundary,
    const std::map<std::string, MtStateTargetWriterInfo>& targetWriters) {
  MtStateTargetWriterInfo info;
  if (boundary.stateTargetNames.size() != 1) return info;
  const std::string& target = *boundary.stateTargetNames.begin();
  auto iter = targetWriters.find(target);
  if (iter == targetWriters.end()) return info;
  info.writerCount = iter->second.writerCount;
  info.multiTargetWriterCount = iter->second.multiTargetWriterCount;
  for (int cppId : iter->second.writerCppIds) {
    if (info.writerCppIds.size() >= MT_STATE_TARGET_WRITER_ID_LIMIT) break;
    info.writerCppIds.insert(cppId);
  }
  return info;
}

static std::string mtStateUpdateTargetWriterConflictKind(
    const MtBoundaryInfo& boundary,
    const MtStateTargetWriterInfo& writerInfo,
    bool hasIncompleteWriterUniverse) {
  if (!boundary.hasStateUpdate || boundary.stateTargetNames.empty()) return "none";
  if (boundary.stateTargetNames.size() != 1) return "multi_target_unproven";
  if (writerInfo.writerCount <= 1 && hasIncompleteWriterUniverse) return "writer_universe_incomplete";
  if (writerInfo.writerCount <= 1) return "unique_writer";
  return "multi_writer_unproven";
}

static std::string mtStateUpdateTargetWriterProof(const std::string& conflictKind) {
  if (conflictKind == "unique_writer") return "target_unique_writer";
  if (conflictKind == "multi_writer_unproven") return "none";
  return "none";
}

static std::vector<std::string> mtStateUpdateRuntimeBlockReasons(
    const std::string& stateUpdateCandidateKind,
    const std::string& targetWriterConflictKind,
    const MtStateTargetWriterInfo& writerInfo) {
  std::vector<std::string> reasons;
  if (stateUpdateCandidateKind != "safe_candidate") addSerialReason(reasons, "not_local_safe_candidate");
  if (targetWriterConflictKind == "multi_writer_unproven") {
    addSerialReason(reasons, "target_multi_writer_unproven");
    if (writerInfo.multiTargetWriterCount > 0) addSerialReason(reasons, "target_multi_target_writer_unproven");
  } else if (targetWriterConflictKind == "multi_target_unproven") {
    addSerialReason(reasons, "target_multi_target_unproven");
  } else if (targetWriterConflictKind == "writer_universe_incomplete") {
    addSerialReason(reasons, "target_writer_universe_incomplete");
  } else if (targetWriterConflictKind != "unique_writer") {
    addSerialReason(reasons, "target_writer_proof_missing");
  }
  return reasons;
}

static std::set<std::string> collectAllMtStateTargetNames(const std::map<int, MtTaskInfo>& mtTasks) {
  std::set<std::string> names;
  for (const auto& iter : mtTasks) {
    const MtBoundaryInfo& boundary = iter.second.boundary;
    names.insert(boundary.stateTargetNames.begin(), boundary.stateTargetNames.end());
  }
  return names;
}

static void applyRepCutLiteSelection(std::map<int, MtTaskInfo>& tasks) {
  int remainingBudget = globalConfig.MtRepCutCopyBudget;
  bool enabled = globalConfig.MtRepCutLiteMode == "on";
  for (auto& iter : tasks) {
    MtTaskInfo& task = iter.second;
    task.repcutSelected = false;
    task.repcutRuntimeApplied = false;
    task.repcutCutInEdges = 0;
    task.repcutCutOutEdges = 0;
    task.repcutBlockReason = repcutBlockReasonForTask(task);

    if (!enabled) {
      if (task.repcutBlockReason.empty()) task.repcutBlockReason = "disabled";
      continue;
    }
    if (globalConfig.MtRepCutCopyBudget <= 0) {
      if (task.repcutBlockReason.empty()) task.repcutBlockReason = "copy_budget_zero";
      continue;
    }
    if (globalConfig.MtRepCutFanoutBudget <= 0) {
      if (task.repcutBlockReason.empty()) task.repcutBlockReason = "fanout_budget_zero";
      continue;
    }
    if (!task.repcutBlockReason.empty()) continue;
    if (task.repcutFanout > globalConfig.MtRepCutFanoutBudget) {
      task.repcutBlockReason = "fanout_budget_exceeded";
      continue;
    }
    if (task.repcutCopyCost > remainingBudget) {
      task.repcutBlockReason = "copy_budget_exceeded";
      continue;
    }
    task.repcutSelected = true;
    remainingBudget -= task.repcutCopyCost;
  }

  for (auto& iter : tasks) {
    int cppId = iter.first;
    MtTaskInfo& task = iter.second;
    if (!enabled || task.repcutSelected || task.taskKind != "pure_compute") continue;
    if (task.hasCandidateCost && task.repcutCopyCost > 0) {
      std::set<int> predCppIds;
      addCppIdsIfExecutable(predCppIds, cppId2Super[cppId]->prev);
      addCppIdsIfExecutable(predCppIds, cppId2Super[cppId]->depPrev);
      bool hasPurePred = false;
      for (int predId : predCppIds) {
        auto pred = tasks.find(predId);
        if (pred != tasks.end() && pred->second.taskKind == "pure_compute") hasPurePred = true;
      }
      if (!hasPurePred) continue;
      if (globalConfig.MtRepCutCopyBudget <= 0) {
        task.repcutBlockReason = "copy_budget_zero";
        continue;
      }
      if (globalConfig.MtRepCutFanoutBudget <= 0) {
        task.repcutBlockReason = "fanout_budget_zero";
        continue;
      }
      if (task.repcutFanout > globalConfig.MtRepCutFanoutBudget) {
        task.repcutBlockReason = "fanout_budget_exceeded";
        continue;
      }
      if (task.repcutCopyCost > remainingBudget) {
        task.repcutBlockReason = "copy_budget_exceeded";
        continue;
      }
      task.repcutSelected = true;
      task.repcutBlockReason.clear();
      remainingBudget -= task.repcutCopyCost;
    }
  }
}

static std::map<int, MtTaskInfo> buildMtTaskInfoMapWithRepCutSelection() {
  std::map<int, MtTaskInfo> tasks = buildMtTaskInfoMapWithRepCut();
  applyRepCutLiteSelection(tasks);
  return tasks;
}

static bool mtTaskUsesRepCutLiteRuntime(const std::map<int, MtTaskInfo>& tasks, int cppId) {
  auto iter = tasks.find(cppId);
  if (iter == tasks.end()) return false;
  return globalConfig.MtHelperMode == "mt" && iter->second.repcutSelected &&
         iter->second.taskKind == "pure_compute";
}

static bool markMtRepCutLiteRuntimeApplied(std::map<int, MtTaskInfo>& tasks) {
  if (globalConfig.MtHelperMode != "mt") return false;
  bool anyApplied = false;
  for (auto& iter : tasks) {
    iter.second.repcutRuntimeApplied = false;
    iter.second.repcutCutInEdges = 0;
    iter.second.repcutCutOutEdges = 0;
  }

  MtPureBatchPlan plan = planMtPureBatches(tasks, globalConfig.MtRepCutLiteMode == "on");
  for (auto batch : plan.batches) {
    for (int cppId = batch.first; cppId < batch.second; cppId ++) {
      auto task = tasks.find(cppId);
      if (task != tasks.end() && mtTaskUsesRepCutLiteRuntime(tasks, cppId)) {
        task->second.repcutRuntimeApplied = true;
        anyApplied = true;
      }
    }
  }
  for (const MtRepCutEdge& edge : plan.cutEdges) {
    tasks[edge.fromCppId].repcutCutOutEdges ++;
    tasks[edge.toCppId].repcutCutInEdges ++;
  }

  return anyApplied;
}

void graph::dumpMtScheduleJson() {
  std::string baseName = globalConfig.InputBaseName.empty() ? name : globalConfig.InputBaseName;
  std::string path = globalConfig.OutputDir + "/" + baseName + "_mt_schedule.json";
  FILE* fp = std::fopen(path.c_str(), "w");
  Assert(fp != nullptr, "failed to open mt schedule json %s", path.c_str());
  std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCut();
  std::set<std::string> allStateTargetNames = collectAllMtStateTargetNames(mtTasks);
  MtStateTargetWriterUniverse stateTargetWriterUniverse = collectMtStateTargetWriters(mtTasks);

  fprintf(fp, "{\n");
  fprintf(fp, "  \"format\": \"gsim.mt-schedule.v1\",\n");
  fprintf(fp, "  \"tasks\": [\n");

  for (int cppId = 0; cppId < superId; cppId ++) {
    SuperNode* super = cppId2Super[cppId];
    int activeWord;
    uint64_t activeMask;
    std::tie(activeWord, activeMask) = setIdxMask(cppId);

    MtTaskInfo& mtTask = mtTasks[cppId];
    const MtBoundaryInfo& boundary = mtTask.boundary;
    std::set<int> predCppIds;
    std::set<int> succCppIds;
    std::set<int> activeFanout;

    addCppIdsIfExecutable(predCppIds, super->prev);
    addCppIdsIfExecutable(predCppIds, super->depPrev);
    addCppIdsIfExecutable(succCppIds, super->next);
    addCppIdsIfExecutable(succCppIds, super->depNext);

    for (Node* member : super->member) {
      for (int nextCppId : member->nextNeedActivate) {
        if (nextCppId >= 0) activeFanout.insert(nextCppId);
      }
    }

    fprintf(fp, "    {\n");
    fprintf(fp, "      \"cpp_id\": %d,\n", cppId);
    fprintf(fp, "      \"scan_index\": %d,\n", cppId);
    fprintf(fp, "      \"super_id\": %d,\n", super->id);
    fprintf(fp, "      \"super_type\": \"%s\",\n", superTypeName(super->superType));
    fprintf(fp, "      \"task_kind\": \"%s\",\n", mtTask.taskKind.c_str());
    fprintf(fp, "      \"serial_reasons\": ");
    dumpJsonStringArray(fp, mtTask.serialReasons);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"active_word\": %d,\n", activeWord);
    fprintf(fp, "      \"active_mask\": \"0x%" PRIx64 "\",\n", activeMask);
    fprintf(fp, "      \"node_kinds\": {");
    bool firstKind = true;
    for (auto iter : boundary.nodeKinds) {
      if (!firstKind) fprintf(fp, ", ");
      firstKind = false;
      fprintf(fp, "\"%s\": %d", iter.first.c_str(), iter.second);
    }
    fprintf(fp, "},\n");

    fprintf(fp, "      \"pred_cpp_ids\": ");
    dumpJsonIntArray(fp, predCppIds);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"succ_cpp_ids\": ");
    dumpJsonIntArray(fp, succCppIds);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"active_fanout\": ");
    dumpJsonIntArray(fp, activeFanout);
    fprintf(fp, ",\n");

    fprintf(fp, "      \"boundary\": {\n");
    fprintf(fp, "        \"has_state_update\": %s,\n", boundary.hasStateUpdate ? "true" : "false");
    fprintf(fp, "        \"has_memory_write\": %s,\n", boundary.hasMemoryWrite ? "true" : "false");
    fprintf(fp, "        \"has_reset\": %s,\n", boundary.hasReset ? "true" : "false");
    fprintf(fp, "        \"has_external\": %s,\n", boundary.hasExternal ? "true" : "false");
    fprintf(fp, "        \"has_special\": %s,\n", boundary.hasSpecial ? "true" : "false");
    fprintf(fp, "        \"clock_names\": ");
    dumpJsonStringArray(fp, boundary.clockNames);
    fprintf(fp, "\n");
    fprintf(fp, "      },\n");

    std::string rhsTimingClass = mtStateUpdateRhsTimingClass(boundary);
    std::string rhsTimingEvidence = mtStateUpdateRhsTimingEvidence(boundary, rhsTimingClass);
    bool rhsReadsSameCycleTarget = mtStateUpdateHasSameCycleTargetRead(boundary, allStateTargetNames);
    bool hasMemoryOrDynamicArray = mtStateUpdateHasMemoryOrDynamicArray(boundary);
    bool hasExternalOrSpecial = mtStateUpdateHasExternalOrSpecial(boundary);
    bool activationCanUseDelta = mtStateUpdateActivationCanUseDelta(boundary);
    std::vector<std::string> stateUpdateBlockReasons = mtStateUpdateBlockReasons(boundary, super, rhsTimingClass, rhsReadsSameCycleTarget);
    std::string stateUpdateCandidateKind = mtStateUpdateCandidateKind(boundary, stateUpdateBlockReasons);
    MtStateTargetWriterInfo stateTargetWriterInfo = mtStateUpdateWriterInfo(boundary, stateTargetWriterUniverse.targetWriters);
    std::string targetWriterConflictKind = mtStateUpdateTargetWriterConflictKind(
        boundary, stateTargetWriterInfo, stateTargetWriterUniverse.hasIncompleteWriterUniverse);
    std::string targetWriterProof = mtStateUpdateTargetWriterProof(targetWriterConflictKind);
    std::vector<std::string> runtimeBlockReasons = mtStateUpdateRuntimeBlockReasons(
        stateUpdateCandidateKind, targetWriterConflictKind, stateTargetWriterInfo);
    bool runtimeSafeCandidate = stateUpdateCandidateKind == "safe_candidate" && runtimeBlockReasons.empty();
    fprintf(fp, "      \"state_update\": {\n");
    fprintf(fp, "        \"has_state_update\": %s,\n", boundary.hasStateUpdate ? "true" : "false");
    fprintf(fp, "        \"state_target_names\": ");
    dumpJsonStringArray(fp, boundary.stateTargetNames);
    fprintf(fp, ",\n");
    fprintf(fp, "        \"state_target_count\": %zu,\n", boundary.stateTargetNames.size());
    fprintf(fp, "        \"single_target\": %s,\n", boundary.stateTargetNames.size() == 1 ? "true" : "false");
    fprintf(fp, "        \"rhs_timing_class\": \"%s\",\n", rhsTimingClass.c_str());
    fprintf(fp, "        \"rhs_timing_evidence\": \"%s\",\n", rhsTimingEvidence.c_str());
    fprintf(fp, "        \"rhs_reads_state_targets\": ");
    dumpJsonStringArray(fp, boundary.rhsReadStateTargetNames);
    fprintf(fp, ",\n");
    fprintf(fp, "        \"rhs_reads_same_cycle_target\": %s,\n", rhsReadsSameCycleTarget ? "true" : "false");
    fprintf(fp, "        \"has_reset_behavior\": %s,\n", boundary.hasReset ? "true" : "false");
    fprintf(fp, "        \"has_async_reset_behavior\": %s,\n", boundary.hasAsyncReset ? "true" : "false");
    fprintf(fp, "        \"has_memory_or_dynamic_array\": %s,\n", hasMemoryOrDynamicArray ? "true" : "false");
    fprintf(fp, "        \"has_external_or_special\": %s,\n", hasExternalOrSpecial ? "true" : "false");
    fprintf(fp, "        \"activation_fanout_count\": %zu,\n", activeFanout.size());
    fprintf(fp, "        \"activation_can_use_delta\": %s,\n", activationCanUseDelta ? "true" : "false");
    fprintf(fp, "        \"candidate_kind\": \"%s\",\n", stateUpdateCandidateKind.c_str());
    fprintf(fp, "        \"block_reasons\": ");
    dumpJsonStringArray(fp, stateUpdateBlockReasons);
    fprintf(fp, "\n");
    fprintf(fp, "      },\n");

    fprintf(fp, "      \"state_update_group\": {\n");
    fprintf(fp, "        \"local_safe_candidate\": %s,\n", stateUpdateCandidateKind == "safe_candidate" ? "true" : "false");
    fprintf(fp, "        \"runtime_safe_candidate\": %s,\n", runtimeSafeCandidate ? "true" : "false");
    fprintf(fp, "        \"target_writer_count\": %d,\n", stateTargetWriterInfo.writerCount);
    fprintf(fp, "        \"target_multi_target_writer_count\": %d,\n", stateTargetWriterInfo.multiTargetWriterCount);
    fprintf(fp, "        \"target_writer_universe_complete\": %s,\n",
            stateTargetWriterUniverse.hasIncompleteWriterUniverse ? "false" : "true");
    fprintf(fp, "        \"target_writer_cpp_ids\": ");
    dumpJsonIntArray(fp, stateTargetWriterInfo.writerCppIds);
    fprintf(fp, ",\n");
    fprintf(fp, "        \"target_writer_conflict_kind\": \"%s\",\n", targetWriterConflictKind.c_str());
    fprintf(fp, "        \"target_writer_proof\": \"%s\",\n", targetWriterProof.c_str());
    fprintf(fp, "        \"runtime_block_reasons\": ");
    dumpJsonStringArray(fp, runtimeBlockReasons);
    fprintf(fp, "\n");
    fprintf(fp, "      },\n");

    fprintf(fp, "      \"repcut\": {\n");
    fprintf(fp, "        \"is_source\": %s,\n", mtTask.isSource ? "true" : "false");
    fprintf(fp, "        \"is_sink\": %s,\n", mtTask.isSink ? "true" : "false");
    if (mtTask.hasCandidateCost) {
      fprintf(fp, "        \"candidate_cost\": %d,\n", mtTask.candidateCost);
    } else {
      fprintf(fp, "        \"candidate_cost\": null,\n");
    }
    fprintf(fp, "        \"repcut_role\": \"%s\",\n", mtTask.repcutRole.c_str());
    fprintf(fp, "        \"repcut_source_count\": %d,\n", mtTask.repcutSourceCount);
    fprintf(fp, "        \"repcut_sink_count\": %d,\n", mtTask.repcutSinkCount);
    fprintf(fp, "        \"repcut_copy_cost\": %d,\n", mtTask.repcutCopyCost);
    fprintf(fp, "        \"repcut_fanout\": %d,\n", mtTask.repcutFanout);
    std::string blockReason = repcutBlockReasonForTask(mtTask);
    if (blockReason.empty()) {
      fprintf(fp, "        \"repcut_block_reason\": null\n");
    } else {
      fprintf(fp, "        \"repcut_block_reason\": \"%s\"\n", jsonEscape(blockReason).c_str());
    }
    fprintf(fp, "      }\n");
    fprintf(fp, "    }%s\n", cppId + 1 == superId ? "" : ",");
  }

  fprintf(fp, "  ]\n");
  fprintf(fp, "}\n");
  fclose(fp);
  printf("[mt-schedule] wrote %d tasks to %s\n", superId, path.c_str());
}

void graph::dumpMtRepCutLiteReport() {
  std::string baseName = globalConfig.InputBaseName.empty() ? name : globalConfig.InputBaseName;
  std::string path = globalConfig.OutputDir + "/" + baseName + "_mt_repcut_lite.json";
  FILE* fp = std::fopen(path.c_str(), "w");
  Assert(fp != nullptr, "failed to open mt repcut-lite report %s", path.c_str());
  std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCutSelection();
  bool appliedToRuntime = markMtRepCutLiteRuntimeApplied(mtTasks);
  MtPureBatchPlan uncutPlan = planMtPureBatches(mtTasks, false);
  MtPureBatchPlan cutPlan = planMtPureBatches(mtTasks, globalConfig.MtRepCutLiteMode == "on");
  MtRepCutSemanticPlan semanticPlan = planMtRepCutSemantics(mtTasks);

  int selectedCount = 0;
  int selectedBoundaryCandidateCount = 0;
  int selectedSinkCount = 0;
  int selectedCost = 0;
  int boundaryCandidateCount = 0;
  int pureCount = 0;
  for (auto& iter : mtTasks) {
    const MtTaskInfo& task = iter.second;
    if (task.taskKind == "pure_compute") pureCount ++;
    if (task.repcutRole == "candidate") boundaryCandidateCount ++;
    if (task.repcutSelected) {
      selectedCount ++;
      selectedCost += task.repcutCopyCost;
      if (task.repcutRole == "candidate") selectedBoundaryCandidateCount ++;
      if (task.repcutRole == "sink") selectedSinkCount ++;
    }
  }

  fprintf(fp, "{\n");
  fprintf(fp, "  \"format\": \"gsim.mt-repcut-lite.v1\",\n");
  fprintf(fp, "  \"mode\": \"%s\",\n", globalConfig.MtRepCutLiteMode.c_str());
  fprintf(fp, "  \"copy_budget\": %d,\n", globalConfig.MtRepCutCopyBudget);
  fprintf(fp, "  \"fanout_budget\": %d,\n", globalConfig.MtRepCutFanoutBudget);
  fprintf(fp, "  \"task_count\": %d,\n", superId);
  fprintf(fp, "  \"pure_task_count\": %d,\n", pureCount);
  fprintf(fp, "  \"boundary_candidate_count\": %d,\n", boundaryCandidateCount);
  fprintf(fp, "  \"candidate_count\": %d,\n", boundaryCandidateCount);
  fprintf(fp, "  \"selected_count\": %d,\n", selectedCount);
  fprintf(fp, "  \"selected_sink_count\": %d,\n", selectedSinkCount);
  fprintf(fp, "  \"selected_boundary_candidate_count\": %d,\n", selectedBoundaryCandidateCount);
  fprintf(fp, "  \"selected_copy_cost\": %d,\n", selectedCost);
  fprintf(fp, "  \"ordering\": \"cpp_id\",\n");
  fprintf(fp, "  \"applied_to_runtime\": %s,\n", appliedToRuntime ? "true" : "false");
  fprintf(fp, "  \"uncut_batch_count\": %d,\n", uncutPlan.segmentCount);
  fprintf(fp, "  \"cut_batch_count\": %d,\n", cutPlan.segmentCount);
  fprintf(fp, "  \"cut_edge_count\": %zu,\n", cutPlan.cutEdges.size());
  fprintf(fp, "  \"cut_edges\": [\n");
  for (size_t i = 0; i < cutPlan.cutEdges.size(); i ++) {
    const MtRepCutEdge& edge = cutPlan.cutEdges[i];
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"from_cpp_id\": %d,\n", edge.fromCppId);
    fprintf(fp, "      \"to_cpp_id\": %d,\n", edge.toCppId);
    fprintf(fp, "      \"reason\": \"%s\"\n", jsonEscape(edge.reason).c_str());
    fprintf(fp, "    }%s\n", i + 1 == cutPlan.cutEdges.size() ? "" : ",");
  }
  fprintf(fp, "  ],\n");
  fprintf(fp, "  \"cut_batches\": [\n");
  for (size_t i = 0; i < semanticPlan.cutBatches.size(); i ++) {
    const MtRepCutBatch& batch = semanticPlan.cutBatches[i];
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"begin_cpp_id\": %d,\n", batch.beginCppId);
    fprintf(fp, "      \"end_cpp_id\": %d,\n", batch.endCppId);
    fprintf(fp, "      \"cut_edge_count\": %d,\n", batch.cutEdgeCount);
    fprintf(fp, "      \"clone_count\": %d,\n", batch.cloneCount);
    fprintf(fp, "      \"forced_sink_cpp_ids\": ");
    dumpJsonIntArray(fp, batch.forcedSinkCppIds);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"forced_sink_mask\": \"0x%lx\",\n", batch.forcedSinkMask);
    fprintf(fp, "      \"forced_sink_activation\": %s,\n", batch.forcedSinkActivation ? "true" : "false");
    fprintf(fp, "      \"forced_serial\": %s,\n", batch.forcedSerial ? "true" : "false");
    fprintf(fp, "      \"parallel_safe\": %s,\n", batch.parallelSafe ? "true" : "false");
    if (!batch.parallelSafeReason.empty()) {
      fprintf(fp, "      \"parallel_safe_reason\": \"%s\"", jsonEscape(batch.parallelSafeReason).c_str());
    } else {
      fprintf(fp, "      \"parallel_safe_reason\": null");
    }
    if (!batch.fallbackReason.empty()) {
      fprintf(fp, ",\n      \"fallback_reason\": \"%s\"\n", jsonEscape(batch.fallbackReason).c_str());
    } else {
      fprintf(fp, ",\n      \"fallback_reason\": null\n");
    }
    fprintf(fp, "    }%s\n", i + 1 == semanticPlan.cutBatches.size() ? "" : ",");
  }
  fprintf(fp, "  ],\n");
  fprintf(fp, "  \"duplicated_nodes\": [\n");
  bool firstClone = true;
  for (const MtRepCutClone& clone : semanticPlan.clones) {
    if (!clone.fallbackReason.empty()) continue;
    if (!firstClone) fprintf(fp, ",\n");
    firstClone = false;
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"source_cpp_id\": %d,\n", clone.sourceCppId);
    fprintf(fp, "      \"sink_cpp_id\": %d,\n", clone.sinkCppId);
    fprintf(fp, "      \"source_node\": \"%s\",\n", jsonEscape(clone.sourceNode ? clone.sourceNode->name : "").c_str());
    fprintf(fp, "      \"clone_name\": \"%s\"\n", jsonEscape(clone.cloneName).c_str());
    fprintf(fp, "    }");
  }
  if (!firstClone) fprintf(fp, "\n");
  fprintf(fp, "  ],\n");
  fprintf(fp, "  \"tasks\": [\n");
  for (int cppId = 0; cppId < superId; cppId ++) {
    const MtTaskInfo& task = mtTasks[cppId];
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"cpp_id\": %d,\n", cppId);
    fprintf(fp, "      \"task_kind\": \"%s\",\n", task.taskKind.c_str());
    fprintf(fp, "      \"repcut_role\": \"%s\",\n", task.repcutRole.c_str());
    fprintf(fp, "      \"repcut_source_count\": %d,\n", task.repcutSourceCount);
    fprintf(fp, "      \"repcut_sink_count\": %d,\n", task.repcutSinkCount);
    fprintf(fp, "      \"repcut_copy_cost\": %d,\n", task.repcutCopyCost);
    fprintf(fp, "      \"repcut_fanout\": %d,\n", task.repcutFanout);
    fprintf(fp, "      \"selected\": %s,\n", task.repcutSelected ? "true" : "false");
    fprintf(fp, "      \"runtime_applied\": %s,\n", task.repcutRuntimeApplied ? "true" : "false");
    fprintf(fp, "      \"cut_in_edges\": %d,\n", task.repcutCutInEdges);
    fprintf(fp, "      \"cut_out_edges\": %d,\n", task.repcutCutOutEdges);
    if (task.repcutBlockReason.empty()) {
      fprintf(fp, "      \"block_reason\": null,\n");
    } else {
      fprintf(fp, "      \"block_reason\": \"%s\",\n", jsonEscape(task.repcutBlockReason).c_str());
    }
    fprintf(fp, "      \"serial_reasons\": ");
    dumpJsonStringArray(fp, task.serialReasons);
    fprintf(fp, "\n");
    fprintf(fp, "    }%s\n", cppId + 1 == superId ? "" : ",");
  }
  fprintf(fp, "  ]\n");
  fprintf(fp, "}\n");
  fclose(fp);
  printf("[mt-repcut-lite] wrote %d tasks (%d selected, cost %d) to %s\n",
         superId, selectedCount, selectedCost, path.c_str());
}

void graph::dumpMtCoarseRegionReport() {
  std::string baseName = globalConfig.InputBaseName.empty() ? name : globalConfig.InputBaseName;
  std::string path = globalConfig.OutputDir + "/" + baseName + "_mt_coarse_regions.json";
  FILE* fp = std::fopen(path.c_str(), "w");
  Assert(fp != nullptr, "failed to open mt coarse-region report %s", path.c_str());
  std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCutSelection();
  MtCoarseRegionPlan coarsePlan = planMtCoarseRegions(mtTasks);
  MtPureBatchPlan fallbackPlan = planMtPureBatchesActiveFrequency(mtTasks, globalConfig.MtRepCutLiteMode == "on");

  int runtimeEligibleCount = 0;
  int maxTaskCount = 0;
  int maxActiveWordSpan = 0;
  int maxParallelWidth = 0;
  std::map<std::string, int> blockerCounts;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (region.runtimeEligible) runtimeEligibleCount ++;
    maxTaskCount = std::max(maxTaskCount, region.taskCount);
    maxActiveWordSpan = std::max(maxActiveWordSpan, region.activeWordSpan);
    maxParallelWidth = std::max(maxParallelWidth, region.estimatedMaxParallelWidth);
    for (const std::string& blocker : region.blockers) blockerCounts[blocker] ++;
  }

  fprintf(fp, "{\n");
  fprintf(fp, "  \"format\": \"gsim.mt-coarse-region-report.v1\",\n");
  fprintf(fp, "  \"mode\": \"%s\",\n", globalConfig.MtBatchFormationMode.c_str());
  fprintf(fp, "  \"coarse_runtime\": \"%s\",\n", globalConfig.MtCoarseRuntimeMode.c_str());
  fprintf(fp, "  \"task_count\": %d,\n", superId);
  fprintf(fp, "  \"active_width\": %d,\n", ACTIVE_WIDTH);
  fprintf(fp, "  \"same_word_fallback_batch_count\": %zu,\n", fallbackPlan.batches.size());
  fprintf(fp, "  \"candidate_region_count\": %zu,\n", coarsePlan.regions.size());
  fprintf(fp, "  \"runtime_eligible_region_count\": %d,\n", runtimeEligibleCount);
  fprintf(fp, "  \"max_task_count\": %d,\n", maxTaskCount);
  fprintf(fp, "  \"max_active_word_span\": %d,\n", maxActiveWordSpan);
  fprintf(fp, "  \"max_parallel_width\": %d,\n", maxParallelWidth);
  fprintf(fp, "  \"blocker_counts\": {");
  bool firstBlocker = true;
  for (const auto& blocker : blockerCounts) {
    if (!firstBlocker) fprintf(fp, ", ");
    firstBlocker = false;
    fprintf(fp, "\"%s\": %d", jsonEscape(blocker.first).c_str(), blocker.second);
  }
  fprintf(fp, "},\n");
  fprintf(fp, "  \"regions\": [\n");
  for (size_t i = 0; i < coarsePlan.regions.size(); i ++) {
    const MtCoarseRegion& region = coarsePlan.regions[i];
    fprintf(fp, "    {\n");
    fprintf(fp, "      \"begin_cpp_id\": %d,\n", region.beginCppId);
    fprintf(fp, "      \"end_cpp_id\": %d,\n", region.endCppId);
    fprintf(fp, "      \"task_count\": %d,\n", region.taskCount);
    fprintf(fp, "      \"active_word_span\": %d,\n", region.activeWordSpan);
    fprintf(fp, "      \"static_cost\": %d,\n", region.staticCost);
    fprintf(fp, "      \"member_node_cost\": %d,\n", region.memberNodeCost);
    fprintf(fp, "      \"expected_active_cost\": %d,\n", region.expectedActiveCost);
    fprintf(fp, "      \"pure_task_count\": %d,\n", region.pureTaskCount);
    fprintf(fp, "      \"serial_blocker_count\": %d,\n", region.serialBlockerCount);
    fprintf(fp, "      \"dependency_edges_inside\": %d,\n", region.dependencyEdgeCount);
    fprintf(fp, "      \"active_visibility_edges\": %d,\n", region.activeVisibilityEdgeCount);
    fprintf(fp, "      \"same_cycle_activation_hazards\": %d,\n", region.sameCycleActivationHazardCount);
    fprintf(fp, "      \"estimated_layer_count\": %d,\n", region.estimatedLayerCount);
    fprintf(fp, "      \"estimated_max_parallel_width\": %d,\n", region.estimatedMaxParallelWidth);
    fprintf(fp, "      \"mtask_count\": %zu,\n", region.mtasks.size());
    fprintf(fp, "      \"bounded_repcut_lite_could_remove_blocking_dependency\": %s,\n", region.repcutLiteCouldHelp ? "true" : "false");
    fprintf(fp, "      \"replication_candidate_count\": %d,\n", region.replicationCandidateCount);
    fprintf(fp, "      \"runtime_eligible\": %s,\n", region.runtimeEligible ? "true" : "false");
    fprintf(fp, "      \"blockers\": ");
    dumpJsonStringArray(fp, region.blockers);
    fprintf(fp, ",\n");
    fprintf(fp, "      \"layers\": [\n");
    for (size_t layerIdx = 0; layerIdx < region.layers.size(); layerIdx ++) {
      const MtCoarseLayer& layer = region.layers[layerIdx];
      fprintf(fp, "        {\"index\": %zu, \"task_cpp_ids\": ", layerIdx);
      dumpJsonIntArray(fp, layer.taskCppIds);
      fprintf(fp, "}%s\n", layerIdx + 1 == region.layers.size() ? "" : ",");
    }
    fprintf(fp, "      ],\n");
    fprintf(fp, "      \"mtasks\": [\n");
    for (size_t mtaskIdx = 0; mtaskIdx < region.mtasks.size(); mtaskIdx ++) {
      const MtCoarseMTask& mtask = region.mtasks[mtaskIdx];
      fprintf(fp, "        {\n");
      fprintf(fp, "          \"index\": %zu,\n", mtaskIdx);
      fprintf(fp, "          \"task_count\": %d,\n", mtask.taskCount);
      fprintf(fp, "          \"ordering_edges_inside\": %d,\n", mtask.orderingEdgeCount);
      fprintf(fp, "          \"layer_task_cpp_ids\": [");
      for (size_t layerIdx = 0; layerIdx < mtask.layerTaskCppIds.size(); layerIdx ++) {
        if (layerIdx != 0) fprintf(fp, ", ");
        dumpJsonIntArray(fp, mtask.layerTaskCppIds[layerIdx]);
      }
      fprintf(fp, "]\n");
      fprintf(fp, "        }%s\n", mtaskIdx + 1 == region.mtasks.size() ? "" : ",");
    }
    fprintf(fp, "      ]\n");
    fprintf(fp, "    }%s\n", i + 1 == coarsePlan.regions.size() ? "" : ",");
  }
  fprintf(fp, "  ]\n");
  fprintf(fp, "}\n");
  fclose(fp);
  printf("[mt-coarse-region] wrote %zu regions (%d runtime eligible) to %s\n",
         coarsePlan.regions.size(), runtimeEligibleCount, path.c_str());
}

std::pair<int, int> cppId2flagIdx(int cppId) {
  int id = cppId / ACTIVE_WIDTH;
  int bit = cppId % ACTIVE_WIDTH;
  return std::make_pair(id, bit);
}

std::pair<int, uint64_t>setIdxMask(int cppId) {
  int id, bit;
  std::tie(id, bit) = cppId2flagIdx(cppId);
  uint64_t mask = (uint64_t)1 << bit;
  return std::make_pair(id, mask);
}

std::pair<int, uint64_t>clearIdxMask(int cppId) {
  int id, bit;
  std::tie(id, bit) = cppId2flagIdx(cppId);
  uint64_t mask = (uint64_t)1 << bit;
  if (ACTIVE_WIDTH == 64) mask = ~mask;
  else mask = (~mask) & (((uint64_t)1 << ACTIVE_WIDTH) - 1);
  return std::make_pair(id, mask);
}

ActiveType activeSet2bitMap(std::set<int>& activeId, std::map<uint64_t, ActiveType>& bitMapInfo, int curId) {
  uint64_t ret = 0;
  std::string comment = "";
  int uniqueIdx = 0;
  for (int id : activeId) {
    if (isAlwaysActive(id)) continue;
    int bitMapId;
    uint64_t bitMapMask;
    std::tie(bitMapId, bitMapMask) = setIdxMask(id);
    int num = 64 / ACTIVE_WIDTH;
    if (curId >= 0 && id > curId && bitMapId == curId / ACTIVE_WIDTH) {
      if (ret == 0) uniqueIdx = id % ACTIVE_WIDTH;
      else uniqueIdx = -1;
      ret |= bitMapMask;
      comment += std::to_string(id) + " ";
    } else {
      int beg = bitMapId - bitMapId % num;
      int end = beg + num;
      int findType = 0;
      uint64_t newMask = bitMapMask << ((bitMapId - beg) * ACTIVE_WIDTH);
      std::string newComment = std::to_string(id);
      if (bitMapInfo.find(bitMapId) != bitMapInfo.end()) {
        ACTIVE_MASK(bitMapInfo[bitMapId]) |= bitMapMask;
        ACTIVE_COMMENT(bitMapInfo[bitMapId]) += " " + std::to_string(id);
        ACTIVE_UNIQUE(bitMapInfo[bitMapId]) = -1;
        findType = 1; // no nothing
      } else {
        for (int newId = beg; newId < end; newId ++) {
          if (bitMapInfo.find(newId) != bitMapInfo.end()) {
            newMask |= ACTIVE_MASK(bitMapInfo[newId]) << ((newId - beg) * ACTIVE_WIDTH);
            newComment += " " + ACTIVE_COMMENT(bitMapInfo[newId]);
            findType = 2;  // find to merge
            bitMapInfo.erase(newId);
          }
        }
      }
      if (findType == 0) bitMapInfo[bitMapId] = std::make_tuple(bitMapMask, std::to_string(id), id % ACTIVE_WIDTH);
      else if (findType == 2) bitMapInfo[beg] = std::make_tuple(newMask, newComment, -1);
    }
  }
  return std::make_tuple(ret, comment, uniqueIdx);
}

std::string updateActiveStr(int idx, uint64_t mask, const std::string& activeBufferName = "") {
  if (!activeBufferName.empty()) return format("%s.orWord(%d, 0x%lx);", activeBufferName.c_str(), idx, mask);
  if (mask <= MAX_U8) return format("activeFlags[%d] |= 0x%lx;", idx, mask);
  if (mask <= MAX_U16) return format("*(uint16_t*)&activeFlags[%d] |= 0x%lx;", idx, mask);
  if (mask <= MAX_U32) return format("*(uint32_t*)&activeFlags[%d] |= 0x%lx;", idx, mask);
  return format("*(uint64_t*)&activeFlags[%d] |= 0x%lx;", idx, mask);
}

std::string updateActiveStr(int idx, uint64_t mask, std::string& cond, int uniqueId, const std::string& activeBufferName = "") {
  if (!activeBufferName.empty()) {
    if (uniqueId >= 0) {
      return format("%s.orWord(%d, %s%s);", activeBufferName.c_str(), idx, cond.c_str(), shiftBits(uniqueId, ShiftDir::Left).c_str());
    }
    int castWidth = 64;
    if (mask <= MAX_U8) castWidth = 8;
    else if (mask <= MAX_U16) castWidth = 16;
    else if (mask <= MAX_U32) castWidth = 32;
    return format("%s.orWord(%d, -(uint%d_t)%s & 0x%lx);", activeBufferName.c_str(), idx, castWidth, cond.c_str(), mask);
  }
  auto activeFlags = std::string("activeFlags[") + std::to_string(idx) + std::string("]");

  if (mask <= MAX_U8) {
    if (uniqueId >= 0) return format("%s |= %s%s;", activeFlags.c_str(), cond.c_str(), shiftBits(uniqueId, ShiftDir::Left).c_str());
    else return format("%s |= -(uint8_t)%s & 0x%lx;", activeFlags.c_str(), cond.c_str(), mask, activeFlags.c_str());
  }
  if (mask <= MAX_U16)
    return format("*(uint16_t*)&%s |= -(uint16_t)%s & 0x%lx;", activeFlags.c_str(), cond.c_str(), mask, activeFlags.c_str());
  if (mask <= MAX_U32)
    return format("*(uint32_t*)&%s |= -(uint32_t)%s & 0x%lx;", activeFlags.c_str(), cond.c_str(), mask, activeFlags.c_str());
  return format("*(uint64_t*)&%s |= -(uint64_t)%s & 0x%lx;", activeFlags.c_str(), cond.c_str(), mask, activeFlags.c_str());
}

static void inline includeLib(FILE* fp, std::string lib, bool isStd) {
  std::string format = isStd ? "#include <%s>\n" : "#include \"%s\"\n";
  fprintf(fp, format.c_str(), lib.c_str());
}

static void inline newLine(FILE* fp) {
  fprintf(fp, "\n");
}

std::string strReplace(std::string s, std::string oldStr, std::string newStr) {
  size_t pos;
  while ((pos = s.find(oldStr)) != std::string::npos) {
    s.replace(pos, oldStr.length(), newStr);
  }
  return s;
}

FILE* graph::genHeaderStart() {
  FILE* header = std::fopen((globalConfig.OutputDir + "/" + name + ".h").c_str(), "w");

  fprintf(header, "#ifndef %s_H\n#define %s_H\n", name.c_str(), name.c_str());
  /* include all libs */
  includeLib(header, "iostream", true);
  includeLib(header, "vector", true);
  includeLib(header, "assert.h", true);
  includeLib(header, "stdlib.h", true);
  includeLib(header, "cstdint", true);
  includeLib(header, "ctime", true);
  includeLib(header, "iomanip", true);
  includeLib(header, "cstring", true);
  includeLib(header, "map", true);
  includeLib(header, "cstdarg", true);
  includeLib(header, "thread", true);
  includeLib(header, "mutex", true);
  includeLib(header, "condition_variable", true);
  includeLib(header, "chrono", true);
  includeLib(header, "cstdlib", true);
  includeLib(header, "algorithm", true);
  newLine(header);

  fprintf(header, "\n// User configuration\n");
  fprintf(header, "//#define ENABLE_LOG\n");
  fprintf(header, "//#define RANDOMIZE_INIT\n");

  fprintf(header, "\n#define gAssert(cond, ...) do {"
                     "if (!(cond)) {"
                       "fprintf(stderr, \"\\33[1;31m\");"
                       "fprintf(stderr, __VA_ARGS__);"
                       "fprintf(stderr, \"\\33[0m\\n\");"
                       "assert(cond);"
                     "}"
                   "} while (0)\n");
  fprintf(header, "#define gdiv(a, b) ((b) == 0 ? 0 : (a) / (b))\n");

  fprintf(header, "#ifndef __BITINT_MAXWIDTH__\n");
  fprintf(header, "#error  BITINT support is required\n");
  fprintf(header, "#endif\n\n");

  /* There is some bugs with _BitInt in clang 18 */
  fprintf(header, "#ifdef __clang__\n");
  fprintf(header, "#if __clang_major__ < 19\n");
  fprintf(header, "#error  Please compile with clang 19 or above\n");
  fprintf(header, "#endif\n");
  fprintf(header, "#endif // __clang__ \n\n");

  fprintf(header, "#define likely(x) __builtin_expect(!!(x), 1)\n");
  fprintf(header, "#define unlikely(x) __builtin_expect(!!(x), 0)\n");
  fprintf(header, "void gprintf(const char *fmt, ...);\n\n");

  for (int num = 2; num <= maxConcatNum; num ++) {
    std::string param;
    for (int i = num; i > 0; i --) param += format(i == num ? "_%d" : ", _%d", i);
    std::string value;
    std::string type = widthUType(num * 64);
    for (int i = num; i > 1; i --) {
      value += format(i == num ? "((%s)_%d << %d) " : "| ((%s)_%d << %d)", type.c_str(), i, (i-1) * 64);
    }
    value += format("| ((%s)_1)", type.c_str());
    fprintf(header, "#define UINT_CONCAT%d(%s) (%s)\n", num, param.c_str(), value.c_str());
  }
  for (std::string str : extDecl) fprintf(header, "%s\n", str.c_str());
  newLine(header);
  return header;
}

void graph::genInterfaceInput(Node* input) {
  /* set by string */
  emitFuncDecl(0, "void S%s::set_%s(%s val) {\n", name.c_str(), input->name.c_str(), widthUType(input->width).c_str());
  emitBodyLock(1, "if (%s != val) { \n", input->name.c_str());
  emitBodyLock(2, "%s = val;\n", input->name.c_str());
  /* update nodes in the same superNode */
  std::set<int> allNext;
  for (Node* next : input->next) {
    if (next->super->cppId >= 0) allNext.insert(next->super->cppId);
  }
  std::map<uint64_t, ActiveType> bitMapInfo;
  activeSet2bitMap(allNext, bitMapInfo, -1);
  for (auto iter : bitMapInfo) {
    emitBodyLock(2, "%s // %s\n", updateActiveStr(iter.first, ACTIVE_MASK(iter.second)).c_str(), ACTIVE_COMMENT(iter.second).c_str());
  }
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");
}

void graph::genInterfaceOutput(Node* output) {
  emitFuncDecl(0, "%s S%s::get_%s() {\n"
               "  return %s;\n"
               "}\n",
               widthUType(output->width).c_str(), name.c_str(),
               output->name.c_str(), output->status == CONSTANT_NODE ? output->computeInfo->valStr.c_str() : output->name.c_str());
}

void graph::genHeaderEnd(FILE* fp) {
  fprintf(fp, "};\n");
  fprintf(fp, "#endif\n");
}

static void emitActiveBufferDef(FILE* header, int activeWords) {
  int packedActiveWords = 64 / ACTIVE_WIDTH;
  fprintf(header,
          "struct ActiveBuffer {\n"
          "  uint%d_t words[%d];\n"
          "  int touchedWords[%d];\n"
          "  int touchedCount;\n"
          "  bool allActive;\n"
          "  ActiveBuffer() : touchedCount(0), allActive(false) {\n"
          "    memset(words, 0, sizeof(words));\n"
          "  }\n"
          "  void clear() {\n"
          "    if (allActive) {\n"
          "      memset(words, 0, sizeof(words));\n"
          "      allActive = false;\n"
          "      touchedCount = 0;\n"
          "      return;\n"
          "    }\n"
          "    for (int touchedIdx = 0; touchedIdx < touchedCount; touchedIdx ++) words[touchedWords[touchedIdx]] = 0;\n"
          "    touchedCount = 0;\n"
          "  }\n"
          "  void orWord(int idx, uint64_t mask) {\n"
          "    // mask packs consecutive active words in little-endian ACTIVE_WIDTH chunks.\n"
          "    for (int i = 0; i < %d && idx + i < %d; i ++) {\n"
          "      uint%d_t value = (uint%d_t)(mask >> (i * %d));\n"
          "      if (value == 0) continue;\n"
          "      int wordIdx = idx + i;\n"
          "      if (!allActive && words[wordIdx] == 0) touchedWords[touchedCount ++] = wordIdx;\n"
          "      words[wordIdx] |= value;\n"
          "    }\n"
          "  }\n"
          "  void activateAll() {\n"
          "    memset(words, 0xff, sizeof(words));\n"
          "    touchedCount = 0;\n"
          "    allActive = true;\n"
          "  }\n"
          "  void mergeFrom(uint%d_t *activeFlags) const {\n"
          "    if (allActive) {\n"
          "      for (int i = 0; i < %d; i ++) activeFlags[i] |= words[i];\n"
          "      return;\n"
          "    }\n"
          "    for (int touchedIdx = 0; touchedIdx < touchedCount; touchedIdx ++) {\n"
          "      int wordIdx = touchedWords[touchedIdx];\n"
          "      activeFlags[wordIdx] |= words[wordIdx];\n"
          "    }\n"
          "  }\n"
          "};\n\n",
          ACTIVE_WIDTH, activeWords, activeWords, packedActiveWords, activeWords, ACTIVE_WIDTH, ACTIVE_WIDTH, ACTIVE_WIDTH, ACTIVE_WIDTH, activeWords);
}

static void emitActivationDeltaDef(FILE* header, int activeWords) {
  int packedActiveWords = 64 / ACTIVE_WIDTH;
  fprintf(header,
          "struct ActivationDeltaEntry {\n"
          "  int idx;\n"
          "  uint64_t mask;\n"
          "};\n"
          "struct ActivationDelta {\n"
          "  std::vector<ActivationDeltaEntry> entries;\n"
          "  bool allActive;\n"
          "  ActivationDelta() : allActive(false) {}\n"
          "  void clear() {\n"
          "    entries.clear();\n"
          "    allActive = false;\n"
          "  }\n"
          "  void orWord(int idx, uint64_t mask) {\n"
          "    // mask packs consecutive active words in little-endian ACTIVE_WIDTH chunks.\n"
          "    for (int i = 0; i < %d && idx + i < %d; i ++) {\n"
          "      uint%d_t value = (uint%d_t)(mask >> (i * %d));\n"
          "      if (value == 0) continue;\n"
          "      int wordIdx = idx + i;\n"
          "      entries.push_back({wordIdx, value});\n"
          "    }\n"
          "  }\n"
          "  void activateAll() {\n"
          "    allActive = true;\n"
          "  }\n"
          "  void mergeInto(uint%d_t *activeFlags) const {\n"
          "    if (allActive) {\n"
          "      for (int i = 0; i < %d; i ++) activeFlags[i] = (uint%d_t)-1;\n"
          "      return;\n"
          "    }\n"
          "    for (const ActivationDeltaEntry &entry : entries) {\n"
          "      activeFlags[entry.idx] |= (uint%d_t)entry.mask;\n"
          "    }\n"
          "  }\n"
          "};\n\n",
          packedActiveWords, activeWords, ACTIVE_WIDTH, ACTIVE_WIDTH, ACTIVE_WIDTH, ACTIVE_WIDTH, activeWords, ACTIVE_WIDTH, ACTIVE_WIDTH);
}

#if defined(DIFFTEST_PER_SIG) && defined(GSIM_DIFF)
void graph::genDiffSig(FILE* fp, Node* node) {
  std::set<std::string> allNames;
  std::string diffNodeName = node->name;
  std::string originName = node->name;
  if (node->type == NODE_MEMORY){

  } else if (node->isArray()) {
    int num = node->arrayEntryNum();
    std::vector<std::string> suffix(num);
    int pairNum = 1;
    for (size_t i = 0; i < node->dimension.size(); i ++) {
      int suffixIdx = 0;
      for (int l = 0; l < pairNum; l ++) {
        for (int j = 0; j < node->dimension[i]; j ++) {
          int suffixNum = num / node->dimension[i];
          for (int k = 0; k < suffixNum; k ++) {
            suffix[suffixIdx] += "[" + std::to_string(j) + "]";
            suffixIdx ++;
          }
        }
      }
      num = num / node->dimension[i];
      pairNum *= node->dimension[i];
    }
    for (size_t i = 0; i < suffix.size(); i ++) {
      allNames.insert(diffNodeName + suffix[i]);
    }
  } else {
    allNames.insert(diffNodeName);
  }
  for (auto iter : allNames)
    fprintf(sigFile, "%d %d %s %s\n", node->sign, node->width, iter.c_str(), iter.c_str());
}
#endif

#if defined(DIFFTEST_PER_SIG) && defined(VERILATOR_DIFF)
void graph::genDiffSig(FILE* fp, Node* node) {
  std::string verilatorName = name + "__DOT__" + node->name;
  size_t pos;
  while ((pos = verilatorName.find("$$")) != std::string::npos) {
    verilatorName.replace(pos, 2, "_");
  }
  while ((pos = verilatorName.find("$")) != std::string::npos) {
    verilatorName.replace(pos, 1, "__DOT__");
  }
  std::map<std::string, std::string> allNames;
  std::string diffNodeName = node->name;
  std::string originName = node->name;
  if (node->type == NODE_MEMORY){

  } else if (node->isArray()) {
    int num = node->arrayEntryNum();
    std::vector<std::string> suffix(num);
    std::vector<std::string> verilatorSuffix(num);
    int pairNum = 1;
    for (size_t i = 0; i < node->dimension.size(); i ++) {
      int suffixIdx = 0;
      for (int l = 0; l < pairNum; l ++) {
        for (int j = 0; j < node->dimension[i]; j ++) {
          int suffixNum = num / node->dimension[i];
          for (int k = 0; k < suffixNum; k ++) {
            verilatorSuffix[suffixIdx] += "_" + std::to_string(j);
            suffix[suffixIdx] += "[" + std::to_string(j) + "]";
            suffixIdx ++;
          }
        }
      }
      num = num / node->dimension[i];
      pairNum *= node->dimension[i];
    }
    for (size_t i = 0; i < suffix.size(); i ++) {
      if (!nameExist(originName + verilatorSuffix[i])) {
        allNames[diffNodeName + suffix[i]] = verilatorName + verilatorSuffix[i];
      }
    }
  } else {
    allNames[diffNodeName] = verilatorName;
  }
  for (auto iter : allNames)
    fprintf(sigFile, "%d %d %s %s\n", node->sign, node->width, iter.first.c_str(), iter.second.c_str());
}
#endif

void graph::genNodeDef(FILE* fp, Node* node) {
  if (node->type == NODE_SPECIAL || node->type == NODE_REG_RESET || (node->status != VALID_NODE)) return;
  if (node->type == NODE_REG_DST && !node->regSplit) return;
  if (node->type == NODE_WRITER) return;
  if (node->isLocal()) return;
#if defined(GSIM_DIFF) || defined(VERILATOR_DIFF)
  genDiffSig(fp, node);
#endif
  if (definedNode.find(node) != definedNode.end()) return;
  definedNode.insert(node);
  fprintf(fp, "%s %s", widthUType(node->width).c_str(), node->name.c_str());
  if (node->type == NODE_MEMORY) fprintf(fp, "[%d]", upperPower2(node->depth));
  for (int dim : node->dimension) fprintf(fp, "[%d]", upperPower2(dim));
  fprintf(fp, "; // width = %d, lineno = %d\n", node->width, node->lineno);
  int w = node->width;
  bool needInitMask = (node->type != NODE_MEMORY && node->type != NODE_WRITER) &&
    (((w < 64) && (w != 8 && w != 16 && w != 32 && w != 64)) || ((w > 64) && (w % 32 != 0)));
  if (needInitMask) {
    if (node->dimension.empty()) {
      emitBodyLock(1, "%s &= %s;\n", node->name.c_str(), bitMask(w).c_str());
    } else {
      int indent = 1;
      int dims = node->dimension.size();
      for (int i = 0; i < dims; i ++) {
        emitBodyLock(indent ++, "for (int i%d = 0; i%d < %d; i%d ++) {\n", i, i, node->dimension[i], i);
      }
      emitBodyLock(indent, "%s", node->name.c_str());
      for (int i = 0; i < dims; i ++) { emitBodyLock(0, "[i%d]", i); }
      emitBodyLock(0, "&= %s;\n", bitMask(w).c_str());
      for (int i = 0; i < dims; i ++) { emitBodyLock(-- indent, "}\n"); }
    }
  }

  /* save reset registers */
  if (node->isReset() && node->type == NODE_REG_SRC) {
    Assert(!node->isArray() && node->width <= BASIC_WIDTH, "%s is treated as reset (isArray: %d width: %d)", node->name.c_str(), node->isArray(), node->width);
    fprintf(fp, "%s %s;\n", widthUType(node->width).c_str(), RESET_NAME(node).c_str());
    if (needInitMask) {
      emitBodyLock(1, "%s = %s & %s;\n", RESET_NAME(node).c_str(), RESET_NAME(node).c_str(), bitMask(w).c_str());
    }
  }
}

void graph::activateNext(Node* node, std::set<int>& nextNodeId, std::string oldName, bool inStep, std::string flagName,
                         std::string activeBufferName, int indent) {
  std::string nodeName = node->name;
  auto condName = std::string("cond_") + nodeName;
  bool opt{false};

  std::map<uint64_t, ActiveType> bitMapInfo;
  ActiveType curMask;
  if (node->isAsyncReset()) {
    emitBodyLock(indent ++, "if (%s || (%s != %s)) {\n", oldName.c_str(), nodeName.c_str(), oldName.c_str());
  } else {
    curMask = activeSet2bitMap(nextNodeId, bitMapInfo, node->super->cppId);
    opt = ((ACTIVE_MASK(curMask) != 0) + bitMapInfo.size()) <= 3;
    if (opt) {
      if (node->width == 1) emitBodyLock(indent, "bool %s = %s ^ %s;\n", condName.c_str(), nodeName.c_str(), oldName.c_str());
      else emitBodyLock(indent, "bool %s = %s != %s;\n", condName.c_str(), nodeName.c_str(), oldName.c_str());
    }
    else {
      emitBodyLock(indent ++, "if (%s != %s) {\n", nodeName.c_str(), oldName.c_str());
    }
  }
  if (inStep) {
    if (node->isReset() && node->type == NODE_REG_SRC) emitBodyLock(indent, "%s = %s;\n", RESET_NAME(node).c_str(), newName(node).c_str());
    emitBodyLock(indent, "%s = %s;\n", node->name.c_str(), newName(node).c_str());
  }
  if (node->isAsyncReset()) {
    Assert(!opt, "invalid opt");
    if (activeBufferName.empty()) emitBodyLock(indent, "activateAll();\n");
    else emitBodyLock(indent, "%s.activateAll();\n", activeBufferName.c_str());
    emitBodyLock(indent, "%s = -1;\n", flagName.c_str());
  } else {
    if (ACTIVE_MASK(curMask) != 0) {
      if (opt) emitBodyLock(indent, "%s |= -(uint%d_t)%s & 0x%lx; // %s\n", flagName.c_str(), ACTIVE_WIDTH, condName.c_str() ,ACTIVE_MASK(curMask), ACTIVE_COMMENT(curMask).c_str());
      else emitBodyLock(indent, "%s |= 0x%lx; // %s\n", flagName.c_str(), ACTIVE_MASK(curMask), ACTIVE_COMMENT(curMask).c_str());
    }
    for (auto iter : bitMapInfo) {
      auto str = opt ? updateActiveStr(iter.first, ACTIVE_MASK(iter.second), condName, ACTIVE_UNIQUE(iter.second), activeBufferName)
                     : updateActiveStr(iter.first, ACTIVE_MASK(iter.second), activeBufferName);
      emitBodyLock(indent, "%s // %s\n", str.c_str(), ACTIVE_COMMENT(iter.second).c_str());
    }
  #ifdef PERF
    #if ENABLE_ACTIVATOR
    for (int id : nextNodeId) {
      emitBodyLock(indent, "if (activator[%d].find(%d) == activator[%d].end()) activator[%d][%d] = 0;\nactivator[%d][%d] ++;\n",
                  id, node->super->cppId, id, id, node->super->cppId, id, node->super->cppId);
    }
    #endif
    if (inStep && node->type != NODE_EXT_OUT) emitBodyLock(indent, "isActivateValid = true;\n");
  #endif
  }
  if (!opt) emitBodyLock(-- indent, "}\n");
}

void graph::activateUncondNext(Node* node, std::set<int>& activateId, bool inStep, std::string flagName,
                               std::string activeBufferName, int indent) {
  std::map<uint64_t, ActiveType> bitMapInfo;
  auto curMask = activeSet2bitMap(activateId, bitMapInfo, node->super->cppId);
  if (ACTIVE_MASK(curMask) != 0) emitBodyLock(indent, "%s |= 0x%lx; // %s\n", flagName.c_str(), ACTIVE_MASK(curMask), ACTIVE_COMMENT(curMask).c_str());
  for (auto iter : bitMapInfo) {
    emitBodyLock(indent, "%s // %s\n", updateActiveStr(iter.first, ACTIVE_MASK(iter.second), activeBufferName).c_str(), ACTIVE_COMMENT(iter.second).c_str());
  }
#ifdef PERF
  #if ENABLE_ACTIVATOR
  for (int id : activateId) {
    emitBodyLock(indent, "if (activator[%d].find(%d) == activator[%d].end()) activator[%d][%d] = 0;\n activator[%d][%d] ++;\n",
                id, node->super->cppId, id, id, node->super->cppId, id, node->super->cppId);
  }
  #endif
  if (inStep) emitBodyLock(indent, "isActivateValid = true;\n");
#endif
}

int graph::genNodeStepStart(SuperNode* node, uint64_t mask, int idx, std::string flagName, int indent) {
  nodeNum ++;
  if (!isAlwaysActive(node->cppId)) {
    emitBodyLock(indent ++, "if(unlikely(%s & 0x%lx)) { // id=%d\n", flagName.c_str(), mask, idx);
  }
  int id;
  uint64_t newMask;
  std::tie(id, newMask) = clearIdxMask(node->cppId);
#ifdef PERF
  emitBodyLock(indent, "activeTimes[%d] ++;\n", node->cppId);
  if (node->superType != SUPER_EXTMOD) {
    emitBodyLock(indent, "bool isActivateValid = false;\n");
  }
#endif
  return indent;
}

void graph::nodeDisplay(Node* member, int indent) {
#define emit_display(varname, width, indent) \
  do { \
    int n = ROUNDUP(width, 64) / 64; \
    std::string s = "printf(\"%%lx"; \
    for (int i = n - 2; i >= 0; i --) { \
      s += "|%%lx"; \
    } \
    s += "\", "; \
    for (n --; n > 0; n --) { \
      s += format("(uint64_t)(%s >> %d)", varname, n * 64); \
      s += ", "; \
    } \
    s += format("(uint64_t)%s",varname);\
    s += ");"; \
    emitBodyLock(indent, s.c_str()); \
  } while (0)

  if (member->status != VALID_NODE) return;
  if (member->type == NODE_WRITER) return;
  emitBodyLock(indent, "printf(\"%%ld %d %s: \", cycles);\n", member->super->cppId, member->name.c_str());
  if (member->dimension.size() != 0) {
    std::string idxStr;
    for (size_t i = 0; i < member->dimension.size(); i ++) {
      emitBodyLock(indent ++, "for(int i%ld = 0; i%ld < %d; i%ld ++) {\n", i, i, member->dimension[i], i);
      idxStr += "[i" + std::to_string(i) + "]";
    }
    std::string nameIdx = member->name + idxStr;
    emit_display(nameIdx.c_str(), member->width, indent);
    emitBodyLock(indent, "printf(\" \");\n");
    for (size_t i = 0; i < member->dimension.size(); i ++) {
      emitBodyLock(-- indent, "}\n");
    }
  } else {
    if (member->anyNextActive() || member->type != NODE_SPECIAL) {
      emit_display(member->name.c_str(), member->width, indent);
    }
  }
  emitBodyLock(indent, "printf(\"\\n\");\n");
}

int graph::genNodeStepEnd(SuperNode* node, int indent) {
#ifdef PERF
  if (node->superType != SUPER_EXTMOD) {
    emitBodyLock(indent, "validActive[%d] += isActivateValid;\n", node->cppId);
  }
#endif

  if(!isAlwaysActive(node->cppId)) {
    emitBodyLock(-- indent, "}\n");
  }
  return indent;
}

bool Node::isLocal() { // TODO: isArray is OK
  return status == VALID_NODE && type == NODE_OTHERS && !anyNextActive() && !isArray() && !isReset();
}

static std::map<Node*, std::string> mtRepCutActiveReplacements;

int graph::translateInst(InstInfo inst, int indent, std::string flagName, std::string activeBufferName) {
  switch (inst.infoType) {
    case SUPER_INFO_IF:
      emitBodyLock(indent ++, "%s\n", mtRepCutReplaceNodeNames(inst.inst, mtRepCutActiveReplacements).c_str());
      break;
    case SUPER_INFO_ELSE:
      emitBodyLock(indent - 1,  "%s\n", mtRepCutReplaceNodeNames(inst.inst, mtRepCutActiveReplacements).c_str());
      break;
    case SUPER_INFO_DEDENT:
      emitBodyLock(--indent, "%s\n", mtRepCutReplaceNodeNames(inst.inst, mtRepCutActiveReplacements).c_str());
      break;
    case SUPER_INFO_STR:
      emitBodyLock(indent, "%s\n", mtRepCutReplaceNodeNames(inst.inst, mtRepCutActiveReplacements).c_str());
      break;
    case SUPER_INFO_ASSIGN_BEG:
      if (inst.node->isLocal() || inst.node->isArray() || inst.node->type == NODE_WRITER) break;
      emitBodyLock(indent, "%s %s = %s;\n", widthUType(inst.node->width).c_str(), oldName(inst.node).c_str(), inst.node->name.c_str());
      break;
    case SUPER_INFO_ASSIGN_END:
      if (inst.node->isLocal() || !inst.node->needActivate()) break;
      if (inst.node->isArray() || inst.node->type == NODE_WRITER) activateUncondNext(inst.node, inst.node->nextActiveId, false, flagName, activeBufferName, indent);
      else activateNext(inst.node, inst.node->nextActiveId, oldName(inst.node), false, flagName, activeBufferName, indent);
      break;
    default:
      break;
  }
  return indent;
}

void graph::genSuperEval(SuperNode* super, std::string flagName, std::string activeBufferName, int indent) { // current indent = 2
  if (super->superType == SUPER_EXTMOD) { // TODO: normalize
    /* save old EXT_OUT*/
    for (size_t i = 1; i < super->member.size(); i ++) {
      if (!super->member[i]->needActivate()) continue;
      Node* extOut = super->member[i];
      emitBodyLock(indent, "%s %s = %s;\n", widthUType(extOut->width).c_str(), oldName(extOut).c_str(), extOut->name.c_str());
    }
    for (InstInfo inst : super->insts) {
      indent = translateInst(inst, indent, flagName, activeBufferName);
    }
    for (size_t i = 1; i < super->member.size(); i ++) {
      if (!super->member[i]->needActivate()) continue;
      if (super->member[i]->isArray()) activateUncondNext(super->member[i], super->member[i]->nextActiveId, false, flagName, activeBufferName, indent);
      else activateNext(super->member[i], super->member[i]->nextActiveId, oldName(super->member[i]), false, flagName, activeBufferName, indent);
    }
  } else {
    if (super->superType == SUPER_ASYNC_RESET) {
      if (activeBufferName.empty()) emitBodyLock(indent, "subReset%d();\n", super2ResetId[super->resetNode].second);
      else emitBodyLock(indent, "subReset%d(%s);\n", super2ResetId[super->resetNode].second, activeBufferName.c_str());
    }
    /* local nodes definition */
    for (Node* n : super->member) {
      if (n->isLocal()) {
        emitBodyLock(indent, "%s %s;\n", widthUType(n->width).c_str(), n->name.c_str());
      }
    }
    for (InstInfo inst : super->insts) {
      indent = translateInst(inst, indent, flagName, activeBufferName);
    }
    if (super->superType == SUPER_ASYNC_RESET) {
      if (activeBufferName.empty()) emitBodyLock(indent, "subReset%d();\n", super2ResetId[super->resetNode].second);
      else emitBodyLock(indent, "subReset%d(%s);\n", super2ResetId[super->resetNode].second, activeBufferName.c_str());
    }
    emitBodyLock(indent, "#ifdef ENABLE_LOG\n");
    emitBodyLock(indent ++, "if (cycles >= LOG_START && cycles <= LOG_END) {\n");
    for (Node* n : super->member) nodeDisplay(n, indent);
    emitBodyLock(-- indent, "}\n");
    emitBodyLock(indent, "#endif\n");
  }
}


int graph::genActivate() {
    emitFuncDecl(0, "void S%s::subStep0() {\n", name.c_str());
    int indent = 1;
    int nextSubStepIdx = 1;
    std::string nextFuncDef = format("void S%s::subStep%d()", name.c_str(), nextSubStepIdx);
    bool prevActiveWhole = false;
    for (int idx = 0; idx < superId; idx ++) {
      int id;
      uint64_t mask;
      std::tie(id, mask) = setIdxMask(idx);
      int offset = idx % ACTIVE_WIDTH;
      if (offset == 0) {
        if (prevActiveWhole) {
          emitBodyLock(--indent, "}\n");
        }
        prevActiveWhole = true;
        for (int j = 0; j < ACTIVE_WIDTH && idx + j < superId; j ++) {
          if (isAlwaysActive(idx + j)) prevActiveWhole = false;
        }
        if (prevActiveWhole) {
          bool newFile = __emitSrc(indent ++, true, false, nextFuncDef.c_str(), "if(unlikely(activeFlags[%d] != 0)) {\n", id);
          if (newFile) {
            nextFuncDef = format("void S%s::subStep%d()", name.c_str(), ++ nextSubStepIdx);
          }
          emitBodyLock(indent, "uint%d_t oldFlag = activeFlags[%d];\n", ACTIVE_WIDTH, id);
          emitBodyLock(indent, "activeFlags[%d] = 0;\n", id);
        }
      }
      SuperNode* super = cppId2Super[idx];
      std::string flagName = prevActiveWhole ? "oldFlag" : format("activeFlags[%d]", id);
      indent = genNodeStepStart(super, mask, idx, flagName, indent);
      genSuperEval(super, flagName, "", indent);
      indent = genNodeStepEnd(super, indent);
    }
    emitBodyLock(--indent, "}\n");
    if (prevActiveWhole) emitBodyLock(--indent, "}\n");

    return nextSubStepIdx - 1; // return the maxinum subStepIdx currently used
}

void graph::genMtTaskHelper(SuperNode* super, bool buffered, const std::string& activeSinkType) {
  if (buffered) {
    emitFuncDecl(0, "void S%s::mtTask%d(uint%d_t &flag, %s &nextActive) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH, activeSinkType.c_str());
    genSuperEval(super, "flag", "nextActive", 1);
  } else {
    emitFuncDecl(0, "void S%s::mtTask%d(uint%d_t &flag) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH);
    genSuperEval(super, "flag", "", 1);
  }
  emitBodyLock(0, "}\n");
}

void graph::genMtRepCutLiteTaskHelper(SuperNode* super, const std::vector<MtRepCutClone>& clones, const std::string& activeSinkType) {
  emitFuncDecl(0, "void S%s::mtRepCutLiteTask%d(uint%d_t &flag, %s &nextActive) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH, activeSinkType.c_str());
  std::map<Node*, std::string> replacements = mtRepCutReplacementMap(clones);
  for (const MtRepCutClone& clone : clones) {
    emitBodyLock(1, "%s %s = %s;\n", widthUType(clone.sourceNode->width).c_str(), clone.cloneName.c_str(), clone.expr.c_str());
  }
  mtRepCutActiveReplacements = replacements;
  genSuperEval(super, "flag", "nextActive", 1);
  mtRepCutActiveReplacements.clear();
  emitBodyLock(0, "}\n");
}

void graph::genMtTaskRunner(const MtRepCutSemanticPlan& semanticPlan) {
  std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCutSelection();
  markMtRepCutLiteRuntimeApplied(mtTasks);
  int shardCount = mtPureBatchShardCount();
  bool useCoarse = globalConfig.MtBatchFormationMode == "coarse";
  auto emitPureTaskSwitchCases = [&](int shardBegin, int shardEnd, bool workerMode) {
    for (int cppId = shardBegin; cppId < shardEnd; cppId ++) {
      if (mtTasks[cppId].taskKind != "pure_compute") continue;
      emitBodyLock(4, "case %d:\n", cppId);
      if (workerMode) {
        emitBodyLock(5, "if (mtWorkerFlags[worker] & 0x%lx) {\n", (uint64_t)1 << (cppId % ACTIVE_WIDTH));
        emitBodyLock(6, "if (mtProfileEnabled) {\n");
        if (mtTasks[cppId].repcutRuntimeApplied) {
          emitBodyLock(7, "mtRepCutLiteTask%d(mtWorkerFlags[worker], mtWorkerDeltas[worker]);\n", cppId);
        } else {
          emitBodyLock(7, "mtTask%d(mtWorkerFlags[worker], mtWorkerDeltas[worker]);\n", cppId);
        }
        emitBodyLock(7, "mtProfileLocalTaskIds[worker].push_back(%d);\n", cppId);
        emitBodyLock(7, "mtProfileLocalWorkerTaskCount[worker] ++;\n");
        emitBodyLock(6, "} else {\n");
        if (mtTasks[cppId].repcutRuntimeApplied) {
          emitBodyLock(7, "mtRepCutLiteTask%d(mtWorkerFlags[worker], mtWorkerDeltas[worker]);\n", cppId);
        } else {
          emitBodyLock(7, "mtTask%d(mtWorkerFlags[worker], mtWorkerDeltas[worker]);\n", cppId);
        }
        emitBodyLock(6, "}\n");
        emitBodyLock(5, "}\n");
      } else {
        emitBodyLock(5, "if (activeWord & 0x%lx) {\n", (uint64_t)1 << (cppId % ACTIVE_WIDTH));
        emitBodyLock(6, "if (mtProfileEnabled) {\n");
        emitBodyLock(7, "std::chrono::steady_clock::time_point mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
        emitBodyLock(7, "mtTask%d(activeWord);\n", cppId);
        emitBodyLock(7, "recordMtProfileTask(%d, true, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileTaskBegin).count());\n", cppId);
        emitBodyLock(7, "mtProfileWorkerTaskCount[0] ++;\n");
        emitBodyLock(6, "} else {\n");
        emitBodyLock(7, "mtTask%d(activeWord);\n", cppId);
        emitBodyLock(6, "}\n");
        emitBodyLock(5, "}\n");
      }
      emitBodyLock(5, "break;\n");
    }
  };
  for (int shard = 0; shard < shardCount; shard ++) {
    int shardBegin = shard * MT_PURE_BATCH_SHARD_SIZE;
    int shardEnd = std::min(superId, shardBegin + MT_PURE_BATCH_SHARD_SIZE);
    emitFuncDecl(0, "void S%s::mtRunPureBatchDirectShard%d(int chunkBegin, int chunkEnd, uint%d_t &activeWord) {\n",
                 name.c_str(), shard, ACTIVE_WIDTH);
    emitBodyLock(1, "if (chunkEnd <= %d || chunkBegin >= %d) return;\n", shardBegin, shardEnd);
    emitBodyLock(1, "int localBegin = std::max(chunkBegin, %d);\n", shardBegin);
    emitBodyLock(1, "int localEnd = std::min(chunkEnd, %d);\n", shardEnd);
    emitBodyLock(1, "for (int cppId = localBegin; cppId < localEnd; cppId ++) {\n");
    emitBodyLock(2, "switch (cppId) {\n");
    emitPureTaskSwitchCases(shardBegin, shardEnd, false);
    emitBodyLock(3, "default:\n");
    emitBodyLock(4, "break;\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(0, "}\n");

    emitFuncDecl(0, "void S%s::mtRunPureBatchWorkerShard%d(int worker, int chunkBegin, int chunkEnd, std::vector<std::vector<int>> &mtProfileLocalTaskIds, std::vector<uint64_t> &mtProfileLocalWorkerTaskCount) {\n",
                 name.c_str(), shard);
    emitBodyLock(1, "if (chunkEnd <= %d || chunkBegin >= %d) return;\n", shardBegin, shardEnd);
    emitBodyLock(1, "int localBegin = std::max(chunkBegin, %d);\n", shardBegin);
    emitBodyLock(1, "int localEnd = std::min(chunkEnd, %d);\n", shardEnd);
    emitBodyLock(1, "for (int cppId = localBegin; cppId < localEnd; cppId ++) {\n");
    emitBodyLock(2, "switch (cppId) {\n");
    emitPureTaskSwitchCases(shardBegin, shardEnd, true);
    emitBodyLock(3, "default:\n");
    emitBodyLock(4, "break;\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(0, "}\n");
  }
  emitFuncDecl(0, "void S%s::mtRunPureBatchWorkerRange(int worker, int chunkBegin, int chunkEnd) {\n", name.c_str());
  emitBodyLock(1, "if (chunkEnd <= chunkBegin) return;\n");
  emitBodyLock(1, "int firstShard = chunkBegin / %d;\n", MT_PURE_BATCH_SHARD_SIZE);
  emitBodyLock(1, "int lastShard = (chunkEnd - 1) / %d;\n", MT_PURE_BATCH_SHARD_SIZE);
  emitBodyLock(1, "for (int shard = firstShard; shard <= lastShard; shard ++) {\n");
  emitBodyLock(2, "switch (shard) {\n");
  for (int shard = 0; shard < shardCount; shard ++) {
    emitBodyLock(3, "case %d:\n", shard);
    emitBodyLock(4, "mtRunPureBatchWorkerShard%d(worker, chunkBegin, chunkEnd, mtProfileLocalTaskIds, mtProfileLocalWorkerTaskCount);\n", shard);
    emitBodyLock(4, "break;\n");
  }
  emitBodyLock(3, "default:\n");
  emitBodyLock(4, "break;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::mtWorkerPoolLoop(int worker) {\n", name.c_str());
  emitBodyLock(1, "uint64_t seenGeneration = 0;\n");
  emitBodyLock(1, "while (true) {\n");
  emitBodyLock(2, "int chunkBegin = 0;\n");
  emitBodyLock(2, "int chunkEnd = 0;\n");
  if (useCoarse) {
    emitBodyLock(2, "int jobKind = 0;\n");
    emitBodyLock(2, "int coarseRegionIndex = -1;\n");
    emitBodyLock(2, "int coarseLayerIndex = -1;\n");
  }
  emitBodyLock(2, "uint64_t generation = 0;\n");
  emitBodyLock(2, "{\n");
  emitBodyLock(3, "std::unique_lock<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(3, "mtWorkerPoolCv.wait(lock, [&]() { return mtWorkerPoolStop || mtWorkerPoolGeneration != seenGeneration; });\n");
  emitBodyLock(3, "if (mtWorkerPoolStop) return;\n");
  emitBodyLock(3, "generation = mtWorkerPoolGeneration;\n");
  emitBodyLock(3, "seenGeneration = generation;\n");
  emitBodyLock(3, "if (worker >= mtWorkerPoolCurrentWorkerCount) continue;\n");
  emitBodyLock(3, "chunkBegin = mtWorkerPoolChunkBegin[worker];\n");
  emitBodyLock(3, "chunkEnd = mtWorkerPoolChunkEnd[worker];\n");
  if (useCoarse) {
    emitBodyLock(3, "jobKind = mtWorkerPoolJobKind;\n");
    emitBodyLock(3, "coarseRegionIndex = mtWorkerPoolCoarseRegionIndex;\n");
    emitBodyLock(3, "coarseLayerIndex = mtWorkerPoolCoarseLayerIndex;\n");
  }
  emitBodyLock(2, "}\n");
  if (useCoarse) {
    emitBodyLock(2, "if (jobKind == 1) mtRunCoarseLayerWorkerRange(worker, coarseRegionIndex, coarseLayerIndex, chunkBegin, chunkEnd);\n");
    emitBodyLock(2, "else if (jobKind == 2) mtRunCoarseMTaskWorkerRange(worker, coarseRegionIndex, chunkBegin, chunkEnd);\n");
    emitBodyLock(2, "else mtRunPureBatchWorkerRange(worker, chunkBegin, chunkEnd);\n");
  } else {
    emitBodyLock(2, "mtRunPureBatchWorkerRange(worker, chunkBegin, chunkEnd);\n");
  }
  emitBodyLock(2, "{\n");
  emitBodyLock(3, "std::lock_guard<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(3, "if (generation == mtWorkerPoolGeneration) {\n");
  emitBodyLock(4, "mtWorkerPoolDoneCount ++;\n");
  emitBodyLock(4, "if (mtWorkerPoolDoneCount >= mtWorkerPoolCurrentWorkerCount) mtWorkerPoolDoneCv.notify_one();\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::startMtWorkerPool() {\n", name.c_str());
  emitBodyLock(1, "if (!mtWorkerPoolEnabled || mtConfiguredWorkerCount <= 1 || !mtWorkerPoolThreads.empty()) return;\n");
  emitBodyLock(1, "mtWorkerPoolThreadCount = mtConfiguredWorkerCount;\n");
  emitBodyLock(1, "mtWorkerPoolChunkBegin.assign((size_t)mtWorkerPoolThreadCount, 0);\n");
  emitBodyLock(1, "mtWorkerPoolChunkEnd.assign((size_t)mtWorkerPoolThreadCount, 0);\n");
  emitBodyLock(1, "mtWorkerDeltas.resize((size_t)mtWorkerPoolThreadCount);\n");
  emitBodyLock(1, "mtWorkerFlags.resize((size_t)mtWorkerPoolThreadCount);\n");
  if (useCoarse) {
    emitBodyLock(1, "mtWorkerCoarseFlags.resize((size_t)mtWorkerPoolThreadCount);\n");
  }
  emitBodyLock(1, "mtWorkerPoolThreads.reserve((size_t)mtWorkerPoolThreadCount);\n");
  emitBodyLock(1, "for (int worker = 0; worker < mtWorkerPoolThreadCount; worker ++) {\n");
  emitBodyLock(2, "mtWorkerPoolThreads.emplace_back([this, worker]() { mtWorkerPoolLoop(worker); });\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::stopMtWorkerPool() {\n", name.c_str());
  emitBodyLock(1, "if (mtWorkerPoolThreads.empty()) return;\n");
  emitBodyLock(1, "{\n");
  emitBodyLock(2, "std::lock_guard<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(2, "mtWorkerPoolStop = true;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "mtWorkerPoolCv.notify_all();\n");
  emitBodyLock(1, "for (std::thread &worker : mtWorkerPoolThreads) {\n");
  emitBodyLock(2, "if (worker.joinable()) worker.join();\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "mtWorkerPoolThreads.clear();\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::mtRunPureBatch(int beginCppId, int endCppId, uint%d_t &activeWord) {\n", name.c_str(), ACTIVE_WIDTH);
  emitBodyLock(1, "int taskCount = endCppId - beginCppId;\n");
  emitBodyLock(1, "if (taskCount <= 0) return;\n");
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileBatchBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "int workerCount = mtConfiguredWorkerCount;\n");
  emitBodyLock(1, "bool mtSkippedBelowMinBatch = false;\n");
  emitBodyLock(1, "bool mtSkippedForcedSerialBatch = false;\n");
  emitBodyLock(1, "if (taskCount < mtMinBatchTasks) {\n");
  emitBodyLock(2, "if (mtProfileEnabled) mtProfileRejectBelowMinBatch ++;\n");
  emitBodyLock(2, "mtSkippedBelowMinBatch = true;\n");
  emitBodyLock(2, "workerCount = 1;\n");
  emitBodyLock(1, "}\n");
  bool hasForcedSerialBatch = false;
  for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
    if (batch.forcedSerial) hasForcedSerialBatch = true;
  }
  if (hasForcedSerialBatch) {
    emitBodyLock(1, "switch (beginCppId) {\n");
    for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
      if (batch.forcedSerial) emitBodyLock(2, "case %d:\n", batch.beginCppId);
    }
    emitBodyLock(3, "mtSkippedForcedSerialBatch = true;\n");
    emitBodyLock(3, "if (mtProfileEnabled) mtProfileRejectDependencyEdge ++;\n");
    emitBodyLock(3, "workerCount = 1;\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(2, "default:\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(1, "}\n");
  }
  emitBodyLock(1, "if (workerCount > taskCount) workerCount = taskCount;\n");
  emitBodyLock(1, "if (workerCount < 2) workerCount = 1;\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "int batchSizeBucket = taskCount <= 1 ? 0 : (taskCount == 2 ? 1 : (taskCount <= 4 ? 2 : (taskCount <= 8 ? 3 : (taskCount <= 15 ? 4 : 5))));\n");
  emitBodyLock(2, "mtProfileBatchSizeHist[batchSizeBucket] ++;\n");
  emitBodyLock(2, "mtProfilePureBatchCount ++;\n");
  emitBodyLock(1, "}\n");
  if (!semanticPlan.batchPlan.batches.empty()) {
    emitBodyLock(1, "if (mtProfileEnabled) {\n");
    emitBodyLock(2, "switch (beginCppId) {\n");
    for (auto batch : semanticPlan.batchPlan.batches) {
      int memberNodeCount = 0;
      int sameActiveWordForwardEdges = 0;
      int crossBatchActivationFanout = 0;
      for (int cppId = batch.first; cppId < batch.second; cppId ++) {
        SuperNode* super = cppId2Super[cppId];
        memberNodeCount += (int)super->member.size();
        for (Node* member : super->member) {
          for (int activeId : member->nextActiveId) {
            if (activeId >= batch.first && activeId < batch.second && activeId > cppId &&
                activeId / ACTIVE_WIDTH == cppId / ACTIVE_WIDTH) sameActiveWordForwardEdges ++;
            if (activeId >= 0 && (activeId < batch.first || activeId >= batch.second)) crossBatchActivationFanout ++;
          }
        }
      }
      emitBodyLock(3, "case %d:\n", batch.first);
      emitBodyLock(4, "mtProfileBatchMemberNodeCount += %d;\n", memberNodeCount);
      emitBodyLock(4, "mtProfileSameActiveWordForwardEdges += %d;\n", sameActiveWordForwardEdges);
      emitBodyLock(4, "mtProfileCrossBatchActivationFanout += %d;\n", crossBatchActivationFanout);
      emitBodyLock(4, "break;\n");
    }
    emitBodyLock(3, "default:\n");
    emitBodyLock(4, "break;\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "}\n");
  }
  emitBodyLock(1, "if (mtProfileEnabled && mtProfileWorkerTaskCount.size() < (size_t)workerCount) mtProfileWorkerTaskCount.resize((size_t)workerCount, 0);\n");
  emitBodyLock(1, "if (workerCount == 1) {\n");
  emitBodyLock(2, "if (mtProfileEnabled) {\n");
  emitBodyLock(3, "mtProfileSkippedFakeParallelBatchCount ++;\n");
  emitBodyLock(3, "if (mtProfileEffectiveWorkerCountHist.size() <= 1) mtProfileEffectiveWorkerCountHist.resize(2, 0);\n");
  emitBodyLock(3, "mtProfileEffectiveWorkerCountHist[1] ++;\n");
  emitBodyLock(3, "if (!mtSkippedBelowMinBatch && !mtSkippedForcedSerialBatch) mtProfileRejectConfiguredSingleWorker ++;\n");
  emitBodyLock(2, "}\n");
  if (!semanticPlan.cutBatches.empty()) {
    emitBodyLock(2, "switch (beginCppId) {\n");
    for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
      uint64_t forcedSinkMask = mtRepCutForcedSinkMaskForBatch(semanticPlan, batch.beginCppId);
      if (forcedSinkMask != 0) {
        emitBodyLock(3, "case %d:\n", batch.beginCppId);
        emitBodyLock(4, "activeWord |= 0x%lx;\n", forcedSinkMask);
        emitBodyLock(4, "break;\n");
      }
    }
    emitBodyLock(3, "default:\n");
    emitBodyLock(4, "break;\n");
    emitBodyLock(2, "}\n");
  }
  emitBodyLock(2, "int firstShard = beginCppId / %d;\n", MT_PURE_BATCH_SHARD_SIZE);
  emitBodyLock(2, "int lastShard = (endCppId - 1) / %d;\n", MT_PURE_BATCH_SHARD_SIZE);
  emitBodyLock(2, "for (int shard = firstShard; shard <= lastShard; shard ++) {\n");
  emitBodyLock(3, "switch (shard) {\n");
  for (int shard = 0; shard < shardCount; shard ++) {
    emitBodyLock(4, "case %d:\n", shard);
    emitBodyLock(5, "mtRunPureBatchDirectShard%d(beginCppId, endCppId, activeWord);\n", shard);
    emitBodyLock(5, "break;\n");
  }
  emitBodyLock(4, "default:\n");
  emitBodyLock(5, "break;\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "if (mtProfileEnabled) mtProfileBatchWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(2, "return;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "mtProfileTrueParallelBatchCount ++;\n");
  emitBodyLock(2, "if (workerCount > mtProfileMaxWorkerCount) mtProfileMaxWorkerCount = workerCount;\n");
  emitBodyLock(2, "if (mtProfileEffectiveWorkerCountHist.size() <= (size_t)workerCount) mtProfileEffectiveWorkerCountHist.resize((size_t)workerCount + 1, 0);\n");
  emitBodyLock(2, "mtProfileEffectiveWorkerCountHist[(size_t)workerCount] ++;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "mtProfileLocalWorkerTaskCount.assign((size_t)workerCount, 0);\n");
  emitBodyLock(2, "mtProfileLocalTaskIds.clear();\n");
  emitBodyLock(2, "mtProfileLocalTaskIds.resize((size_t)workerCount);\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtWorkerDeltas.size() < (size_t)workerCount) mtWorkerDeltas.resize((size_t)workerCount);\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) mtWorkerDeltas[worker].clear();\n");
  emitBodyLock(1, "if (mtWorkerFlags.size() < (size_t)workerCount) mtWorkerFlags.resize((size_t)workerCount);\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) mtWorkerFlags[worker] = activeWord;\n");
  if (!semanticPlan.cutBatches.empty()) {
    emitBodyLock(1, "switch (beginCppId) {\n");
    for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
      uint64_t forcedSinkMask = mtRepCutForcedSinkMaskForBatch(semanticPlan, batch.beginCppId);
      if (forcedSinkMask != 0) {
        emitBodyLock(2, "case %d:\n", batch.beginCppId);
        emitBodyLock(3, "for (int worker = 0; worker < workerCount; worker ++) mtWorkerFlags[worker] |= 0x%lx;\n", forcedSinkMask);
        emitBodyLock(3, "break;\n");
      }
    }
    emitBodyLock(2, "default:\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(1, "}\n");
  }
  emitBodyLock(1, "if (workerCount == 1) {\n");
  emitBodyLock(2, "mtRunPureBatchWorkerRange(0, beginCppId, endCppId);\n");
  emitBodyLock(1, "} else if (mtWorkerPoolEnabled && mtWorkerPoolThreadCount >= workerCount) {\n");
  emitBodyLock(2, "{\n");
  emitBodyLock(3, "std::lock_guard<std::mutex> lock(mtWorkerPoolMutex);\n");
  if (useCoarse) {
    emitBodyLock(3, "mtWorkerPoolJobKind = 0;\n");
  }
  emitBodyLock(3, "mtWorkerPoolCurrentWorkerCount = workerCount;\n");
  emitBodyLock(3, "mtWorkerPoolDoneCount = 0;\n");
  emitBodyLock(3, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(4, "mtWorkerPoolChunkBegin[worker] = beginCppId + (taskCount * worker) / workerCount;\n");
  emitBodyLock(4, "mtWorkerPoolChunkEnd[worker] = beginCppId + (taskCount * (worker + 1)) / workerCount;\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(3, "mtWorkerPoolGeneration ++;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "mtWorkerPoolCv.notify_all();\n");
  emitBodyLock(2, "{\n");
  emitBodyLock(3, "std::unique_lock<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(3, "mtWorkerPoolDoneCv.wait(lock, [&]() { return mtWorkerPoolDoneCount >= mtWorkerPoolCurrentWorkerCount; });\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "} else {\n");
  emitBodyLock(2, "std::vector<std::thread> workers;\n");
  emitBodyLock(2, "workers.reserve(workerCount);\n");
  emitBodyLock(2, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(3, "int chunkBegin = beginCppId + (taskCount * worker) / workerCount;\n");
  emitBodyLock(3, "int chunkEnd = beginCppId + (taskCount * (worker + 1)) / workerCount;\n");
  emitBodyLock(3, "workers.emplace_back([&, worker, chunkBegin, chunkEnd]() { mtRunPureBatchWorkerRange(worker, chunkBegin, chunkEnd); });\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "for (std::thread &worker : workers) worker.join();\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileMergeBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) activeWord |= mtWorkerFlags[worker];\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) mtWorkerDeltas[worker].mergeInto(activeFlags);\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(3, "mtProfileActivationDeltaEntries += mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(3, "if (mtWorkerDeltas[worker].entries.size() > mtProfileActivationDeltaMaxEntriesPerWorker) mtProfileActivationDeltaMaxEntriesPerWorker = mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(3, "if (mtWorkerDeltas[worker].allActive) mtProfileActivationDeltaActivateAllCount ++;\n");
  emitBodyLock(3, "mtProfileWorkerTaskCount[(size_t)worker] += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(3, "mtProfilePureTasks += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(3, "for (int cppId : mtProfileLocalTaskIds[worker]) mtProfileTaskExecCount[cppId] ++;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileMergeBegin).count();\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileTrueParallelWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(0, "}\n");
}

void graph::genMtCoarseRegionRunner(const MtRepCutSemanticPlan& semanticPlan, const MtCoarseRegionPlan& coarsePlan) {
  std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCutSelection();
  markMtRepCutLiteRuntimeApplied(mtTasks);

  emitFuncDecl(0, "void S%s::mtRunCoarseLayerWorkerRange(int worker, int regionIndex, int layerIndex, int chunkBegin, int chunkEnd) {\n", name.c_str());
  emitBodyLock(1, "if (chunkEnd <= chunkBegin) return;\n");
  emitBodyLock(1, "switch (regionIndex) {\n");
  int regionIndex = 0;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (!region.runtimeEligible) continue;
    emitBodyLock(2, "case %d:\n", regionIndex);
    emitBodyLock(3, "switch (layerIndex) {\n");
    for (size_t layerIdx = 0; layerIdx < region.layers.size(); layerIdx ++) {
      const MtCoarseLayer& layer = region.layers[layerIdx];
      emitBodyLock(4, "case %zu:\n", layerIdx);
      emitBodyLock(5, "for (int localIndex = chunkBegin; localIndex < chunkEnd; localIndex ++) {\n");
      emitBodyLock(6, "switch (localIndex) {\n");
      for (size_t localIndex = 0; localIndex < layer.taskCppIds.size(); localIndex ++) {
        int cppId = layer.taskCppIds[localIndex];
        int wordOffset = cppId / ACTIVE_WIDTH - region.beginActiveWord;
        uint64_t mask = (uint64_t)1 << (cppId % ACTIVE_WIDTH);
        emitBodyLock(7, "case %zu:\n", localIndex);
        emitBodyLock(8, "if (mtWorkerCoarseFlags[worker][%d] & 0x%lx) {\n", wordOffset, mask);
        if (mtTasks[cppId].repcutRuntimeApplied) {
          emitBodyLock(9, "mtRepCutLiteTask%d(mtWorkerCoarseFlags[worker][%d], mtWorkerDeltas[worker]);\n", cppId, wordOffset);
        } else {
          emitBodyLock(9, "mtTask%d(mtWorkerCoarseFlags[worker][%d], mtWorkerDeltas[worker]);\n", cppId, wordOffset);
        }
        emitBodyLock(9, "if (mtProfileEnabled) {\n");
        emitBodyLock(10, "mtProfileLocalTaskIds[worker].push_back(%d);\n", cppId);
        emitBodyLock(10, "mtProfileLocalWorkerTaskCount[worker] ++;\n");
        emitBodyLock(9, "}\n");
        emitBodyLock(8, "}\n");
        emitBodyLock(8, "break;\n");
      }
      emitBodyLock(7, "default:\n");
      emitBodyLock(8, "break;\n");
      emitBodyLock(6, "}\n");
      emitBodyLock(5, "}\n");
      emitBodyLock(5, "break;\n");
    }
    emitBodyLock(4, "default:\n");
    emitBodyLock(5, "break;\n");
    emitBodyLock(3, "}\n");
    emitBodyLock(3, "break;\n");
    regionIndex ++;
  }
  emitBodyLock(2, "default:\n");
  emitBodyLock(3, "break;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::mtMergeLocalCoarseDelta(int worker, int regionBeginActiveWord, int regionActiveWordSpan) {\n", name.c_str());
  emitBodyLock(1, "if (mtWorkerDeltas[worker].allActive) {\n");
  emitBodyLock(2, "for (int word = 0; word < regionActiveWordSpan; word ++) mtWorkerCoarseFlags[worker][word] = (uint%d_t)-1;\n", ACTIVE_WIDTH);
  emitBodyLock(2, "return;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "size_t entryCountBeforeMerge = mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(1, "size_t localEntryCount = 0;\n");
  emitBodyLock(1, "size_t writeIndex = 0;\n");
  emitBodyLock(1, "for (size_t readIndex = 0; readIndex < mtWorkerDeltas[worker].entries.size(); readIndex ++) {\n");
  emitBodyLock(2, "const ActivationDeltaEntry &entry = mtWorkerDeltas[worker].entries[readIndex];\n");
  emitBodyLock(2, "int localWord = entry.idx - regionBeginActiveWord;\n");
  emitBodyLock(2, "if (localWord >= 0 && localWord < regionActiveWordSpan) {\n");
  emitBodyLock(3, "mtWorkerCoarseFlags[worker][localWord] |= (uint%d_t)entry.mask;\n", ACTIVE_WIDTH);
  emitBodyLock(3, "localEntryCount ++;\n");
  emitBodyLock(2, "} else {\n");
  emitBodyLock(3, "mtWorkerDeltas[worker].entries[writeIndex ++] = entry;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "mtWorkerDeltas[worker].entries.resize(writeIndex);\n");
  emitBodyLock(1, "if (mtProfileEnabled && localEntryCount > 0) {\n");
  emitBodyLock(2, "mtProfileLocalActivationDeltaEntries[worker] += localEntryCount;\n");
  emitBodyLock(2, "if (entryCountBeforeMerge > mtProfileLocalActivationDeltaMaxEntries[worker]) mtProfileLocalActivationDeltaMaxEntries[worker] = entryCountBeforeMerge;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::mtRunCoarseMTaskWorkerRange(int worker, int regionIndex, int mtaskBegin, int mtaskEnd) {\n", name.c_str());
  emitBodyLock(1, "if (mtaskEnd <= mtaskBegin) return;\n");
  emitBodyLock(1, "switch (regionIndex) {\n");
  regionIndex = 0;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (!region.runtimeEligible) continue;
    emitBodyLock(2, "case %d:\n", regionIndex);
    emitBodyLock(3, "for (int mtaskIndex = mtaskBegin; mtaskIndex < mtaskEnd; mtaskIndex ++) {\n");
    emitBodyLock(4, "switch (mtaskIndex) {\n");
    for (size_t mtaskIdx = 0; mtaskIdx < region.mtasks.size(); mtaskIdx ++) {
      const MtCoarseMTask& mtask = region.mtasks[mtaskIdx];
      emitBodyLock(5, "case %zu:\n", mtaskIdx);
      for (size_t layerIdx = 0; layerIdx < mtask.layerTaskCppIds.size(); layerIdx ++) {
        const std::vector<int>& taskCppIds = mtask.layerTaskCppIds[layerIdx];
        if (taskCppIds.empty()) continue;
        emitBodyLock(6, "{\n");
        for (int cppId : taskCppIds) {
          int wordOffset = cppId / ACTIVE_WIDTH - region.beginActiveWord;
          uint64_t mask = (uint64_t)1 << (cppId % ACTIVE_WIDTH);
          emitBodyLock(7, "if (mtWorkerCoarseFlags[worker][%d] & 0x%lx) {\n", wordOffset, mask);
          if (mtTasks[cppId].repcutRuntimeApplied) {
            emitBodyLock(8, "mtRepCutLiteTask%d(mtWorkerCoarseFlags[worker][%d], mtWorkerDeltas[worker]);\n", cppId, wordOffset);
          } else {
            emitBodyLock(8, "mtTask%d(mtWorkerCoarseFlags[worker][%d], mtWorkerDeltas[worker]);\n", cppId, wordOffset);
          }
          emitBodyLock(8, "if (mtProfileEnabled) {\n");
          emitBodyLock(9, "mtProfileLocalTaskIds[worker].push_back(%d);\n", cppId);
          emitBodyLock(9, "mtProfileLocalWorkerTaskCount[worker] ++;\n");
          emitBodyLock(8, "}\n");
          emitBodyLock(7, "}\n");
        }
        emitBodyLock(7, "mtMergeLocalCoarseDelta(worker, %d, %d);\n", region.beginActiveWord, region.activeWordSpan);
        emitBodyLock(6, "}\n");
      }
      emitBodyLock(6, "break;\n");
    }
    emitBodyLock(5, "default:\n");
    emitBodyLock(6, "break;\n");
    emitBodyLock(4, "}\n");
    emitBodyLock(3, "}\n");
    emitBodyLock(3, "break;\n");
    regionIndex ++;
  }
  emitBodyLock(2, "default:\n");
  emitBodyLock(3, "break;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::mtRunCoarseRegion(int regionIndex, uint%d_t *coarseActiveWords) {\n", name.c_str(), ACTIVE_WIDTH);
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileBatchBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "int regionTaskCount = 0;\n");
  emitBodyLock(1, "int regionBeginActiveWord = 0;\n");
  emitBodyLock(1, "int regionActiveWordSpan = 0;\n");
  emitBodyLock(1, "int regionLayerCount = 0;\n");
  emitBodyLock(1, "int regionMemberNodeCount = 0;\n");
  emitBodyLock(1, "switch (regionIndex) {\n");
  regionIndex = 0;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (!region.runtimeEligible) continue;
    emitBodyLock(2, "case %d:\n", regionIndex);
    emitBodyLock(3, "regionTaskCount = %d;\n", region.taskCount);
    emitBodyLock(3, "regionBeginActiveWord = %d;\n", region.beginActiveWord);
    emitBodyLock(3, "regionActiveWordSpan = %d;\n", region.activeWordSpan);
    emitBodyLock(3, "regionLayerCount = %d;\n", region.estimatedLayerCount);
    emitBodyLock(3, "regionMemberNodeCount = %d;\n", region.memberNodeCost);
    emitBodyLock(3, "break;\n");
    regionIndex ++;
  }
  emitBodyLock(2, "default:\n");
  emitBodyLock(3, "return;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "mtProfileCoarseRegionInvocations ++;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "int workerCount = mtConfiguredWorkerCount;\n");
  emitBodyLock(1, "if (workerCount > regionTaskCount) workerCount = regionTaskCount;\n");
  emitBodyLock(1, "if (workerCount < 2) workerCount = 1;\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "int batchSizeBucket = regionTaskCount <= 1 ? 0 : (regionTaskCount == 2 ? 1 : (regionTaskCount <= 4 ? 2 : (regionTaskCount <= 8 ? 3 : (regionTaskCount <= 15 ? 4 : 5))));\n");
  emitBodyLock(2, "mtProfileBatchSizeHist[batchSizeBucket] ++;\n");
  emitBodyLock(2, "mtProfilePureBatchCount ++;\n");
  emitBodyLock(2, "mtProfileBatchMemberNodeCount += regionMemberNodeCount;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled && mtProfileWorkerTaskCount.size() < (size_t)workerCount) mtProfileWorkerTaskCount.resize((size_t)workerCount, 0);\n");
  emitBodyLock(1, "if (workerCount == 1) {\n");
  emitBodyLock(2, "if (mtProfileEnabled) {\n");
  emitBodyLock(3, "mtProfileSkippedFakeParallelBatchCount ++;\n");
  emitBodyLock(3, "if (mtProfileEffectiveWorkerCountHist.size() <= 1) mtProfileEffectiveWorkerCountHist.resize(2, 0);\n");
  emitBodyLock(3, "mtProfileEffectiveWorkerCountHist[1] ++;\n");
  emitBodyLock(3, "mtProfileRejectConfiguredSingleWorker ++;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "} else if (mtProfileEnabled) {\n");
  emitBodyLock(2, "mtProfileTrueParallelBatchCount ++;\n");
  emitBodyLock(2, "if (workerCount > mtProfileMaxWorkerCount) mtProfileMaxWorkerCount = workerCount;\n");
  emitBodyLock(2, "if (mtProfileEffectiveWorkerCountHist.size() <= (size_t)workerCount) mtProfileEffectiveWorkerCountHist.resize((size_t)workerCount + 1, 0);\n");
  emitBodyLock(2, "mtProfileEffectiveWorkerCountHist[(size_t)workerCount] ++;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtWorkerDeltas.size() < (size_t)workerCount) mtWorkerDeltas.resize((size_t)workerCount);\n");
  emitBodyLock(1, "if (mtWorkerCoarseFlags.size() < (size_t)workerCount) mtWorkerCoarseFlags.resize((size_t)workerCount);\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "mtProfileLocalWorkerTaskCount.assign((size_t)workerCount, 0);\n");
  emitBodyLock(2, "mtProfileLocalTaskIds.clear();\n");
  emitBodyLock(2, "mtProfileLocalTaskIds.resize((size_t)workerCount);\n");
  emitBodyLock(2, "mtProfileLocalActivationDeltaEntries.assign((size_t)workerCount, 0);\n");
  emitBodyLock(2, "mtProfileLocalActivationDeltaMaxEntries.assign((size_t)workerCount, 0);\n");
  emitBodyLock(1, "}\n");
  if (globalConfig.MtCoarseRuntimeMode == "mtask") {
    emitBodyLock(1, "int regionMTaskCount = 0;\n");
    emitBodyLock(1, "switch (regionIndex) {\n");
    regionIndex = 0;
    for (const MtCoarseRegion& region : coarsePlan.regions) {
      if (!region.runtimeEligible) continue;
      emitBodyLock(2, "case %d:\n", regionIndex);
      emitBodyLock(3, "regionMTaskCount = %zu;\n", region.mtasks.size());
      emitBodyLock(3, "break;\n");
      regionIndex ++;
    }
    emitBodyLock(2, "default:\n");
    emitBodyLock(3, "regionMTaskCount = 0;\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "if (regionMTaskCount <= 0) return;\n");
    emitBodyLock(1, "int mtaskWorkerCount = workerCount;\n");
    emitBodyLock(1, "if (mtaskWorkerCount > regionMTaskCount) mtaskWorkerCount = regionMTaskCount;\n");
    emitBodyLock(1, "if (mtaskWorkerCount < 1) mtaskWorkerCount = 1;\n");
    emitBodyLock(1, "for (int worker = 0; worker < mtaskWorkerCount; worker ++) {\n");
    emitBodyLock(2, "mtWorkerDeltas[worker].clear();\n");
    emitBodyLock(2, "mtWorkerCoarseFlags[worker].assign(coarseActiveWords, coarseActiveWords + regionActiveWordSpan);\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "if (mtProfileEnabled) {\n");
    emitBodyLock(2, "mtProfileCoarseMTaskDispatches += regionMTaskCount;\n");
    emitBodyLock(2, "mtProfileCoarseWorkerJobs += mtaskWorkerCount;\n");
    emitBodyLock(2, "mtProfileCoarseFlagWordCopies += (uint64_t)mtaskWorkerCount * (uint64_t)regionActiveWordSpan;\n");
    emitBodyLock(2, "mtProfileCoarseEstimatedBarrierCount += 1;\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "if (mtaskWorkerCount == 1) {\n");
    emitBodyLock(2, "mtRunCoarseMTaskWorkerRange(0, regionIndex, 0, regionMTaskCount);\n");
    emitBodyLock(1, "} else if (mtWorkerPoolEnabled && mtWorkerPoolThreadCount >= mtaskWorkerCount) {\n");
    emitBodyLock(2, "{\n");
    emitBodyLock(3, "std::lock_guard<std::mutex> lock(mtWorkerPoolMutex);\n");
    emitBodyLock(3, "mtWorkerPoolJobKind = 2;\n");
    emitBodyLock(3, "mtWorkerPoolCoarseRegionIndex = regionIndex;\n");
    emitBodyLock(3, "mtWorkerPoolCoarseLayerIndex = -1;\n");
    emitBodyLock(3, "mtWorkerPoolCurrentWorkerCount = mtaskWorkerCount;\n");
    emitBodyLock(3, "mtWorkerPoolDoneCount = 0;\n");
    emitBodyLock(3, "for (int worker = 0; worker < mtaskWorkerCount; worker ++) {\n");
    emitBodyLock(4, "mtWorkerPoolChunkBegin[worker] = (regionMTaskCount * worker) / mtaskWorkerCount;\n");
    emitBodyLock(4, "mtWorkerPoolChunkEnd[worker] = (regionMTaskCount * (worker + 1)) / mtaskWorkerCount;\n");
    emitBodyLock(3, "}\n");
    emitBodyLock(3, "mtWorkerPoolGeneration ++;\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(2, "mtWorkerPoolCv.notify_all();\n");
    emitBodyLock(2, "{\n");
    emitBodyLock(3, "std::unique_lock<std::mutex> lock(mtWorkerPoolMutex);\n");
    emitBodyLock(3, "mtWorkerPoolDoneCv.wait(lock, [&]() { return mtWorkerPoolDoneCount >= mtWorkerPoolCurrentWorkerCount; });\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "} else {\n");
    emitBodyLock(2, "std::vector<std::thread> workers;\n");
    emitBodyLock(2, "workers.reserve(mtaskWorkerCount);\n");
    emitBodyLock(2, "for (int worker = 0; worker < mtaskWorkerCount; worker ++) {\n");
    emitBodyLock(3, "int mtaskBegin = (regionMTaskCount * worker) / mtaskWorkerCount;\n");
    emitBodyLock(3, "int mtaskEnd = (regionMTaskCount * (worker + 1)) / mtaskWorkerCount;\n");
    emitBodyLock(3, "workers.emplace_back([&, worker, mtaskBegin, mtaskEnd]() { mtRunCoarseMTaskWorkerRange(worker, regionIndex, mtaskBegin, mtaskEnd); });\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(2, "for (std::thread &worker : workers) worker.join();\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileMergeBegin;\n");
    emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeBegin = std::chrono::steady_clock::now();\n");
    emitBodyLock(1, "for (int worker = 0; worker < mtaskWorkerCount; worker ++) {\n");
    emitBodyLock(2, "for (int word = 0; word < regionActiveWordSpan; word ++) coarseActiveWords[word] |= mtWorkerCoarseFlags[worker][word];\n");
    emitBodyLock(2, "for (const ActivationDeltaEntry &entry : mtWorkerDeltas[worker].entries) {\n");
    emitBodyLock(3, "int localWord = entry.idx - regionBeginActiveWord;\n");
    emitBodyLock(3, "if (localWord >= 0 && localWord < regionActiveWordSpan) coarseActiveWords[localWord] |= (uint%d_t)entry.mask;\n", ACTIVE_WIDTH);
    emitBodyLock(3, "else activeFlags[entry.idx] |= (uint%d_t)entry.mask;\n", ACTIVE_WIDTH);
    emitBodyLock(2, "}\n");
    emitBodyLock(2, "if (mtWorkerDeltas[worker].allActive) {\n");
    emitBodyLock(3, "for (int word = 0; word < regionActiveWordSpan; word ++) coarseActiveWords[word] = (uint%d_t)-1;\n", ACTIVE_WIDTH);
    emitBodyLock(3, "for (int word = 0; word < %d; word ++) activeFlags[word] = (uint%d_t)-1;\n", activeFlagNum, ACTIVE_WIDTH);
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "if (mtProfileEnabled) {\n");
    emitBodyLock(2, "mtProfileCoarseMergeWordScans += (uint64_t)mtaskWorkerCount * (uint64_t)regionActiveWordSpan;\n");
    emitBodyLock(2, "for (int worker = 0; worker < mtaskWorkerCount; worker ++) {\n");
    emitBodyLock(3, "mtProfileCoarseActivationDeltaEntries += mtProfileLocalActivationDeltaEntries[worker];\n");
    emitBodyLock(3, "mtProfileActivationDeltaEntries += mtProfileLocalActivationDeltaEntries[worker];\n");
    emitBodyLock(3, "mtProfileCoarseActivationDeltaEntries += mtWorkerDeltas[worker].entries.size();\n");
    emitBodyLock(3, "mtProfileActivationDeltaEntries += mtWorkerDeltas[worker].entries.size();\n");
    emitBodyLock(3, "if (mtProfileLocalActivationDeltaMaxEntries[worker] > mtProfileActivationDeltaMaxEntriesPerWorker) mtProfileActivationDeltaMaxEntriesPerWorker = mtProfileLocalActivationDeltaMaxEntries[worker];\n");
    emitBodyLock(3, "if (mtWorkerDeltas[worker].entries.size() > mtProfileActivationDeltaMaxEntriesPerWorker) mtProfileActivationDeltaMaxEntriesPerWorker = mtWorkerDeltas[worker].entries.size();\n");
    emitBodyLock(3, "if (mtWorkerDeltas[worker].allActive) mtProfileActivationDeltaActivateAllCount ++;\n");
    emitBodyLock(3, "mtProfileWorkerTaskCount[(size_t)worker] += mtProfileLocalWorkerTaskCount[worker];\n");
    emitBodyLock(3, "mtProfilePureTasks += mtProfileLocalWorkerTaskCount[worker];\n");
    emitBodyLock(3, "for (int cppId : mtProfileLocalTaskIds[worker]) mtProfileTaskExecCount[cppId] ++;\n");
    emitBodyLock(2, "}\n");
    emitBodyLock(1, "}\n");
    emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileMergeBegin).count();\n");
    emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
    emitBodyLock(1, "if (mtProfileEnabled && workerCount > 1) mtProfileTrueParallelWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
    emitBodyLock(1, "return;\n");
  }
  emitBodyLock(1, "for (int layer = 0; layer < regionLayerCount; layer ++) {\n");
  emitBodyLock(2, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(3, "mtWorkerDeltas[worker].clear();\n");
  emitBodyLock(3, "mtWorkerCoarseFlags[worker].assign(coarseActiveWords, coarseActiveWords + regionActiveWordSpan);\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "int layerTaskCount = 0;\n");
  emitBodyLock(2, "switch (regionIndex) {\n");
  regionIndex = 0;
  for (const MtCoarseRegion& region : coarsePlan.regions) {
    if (!region.runtimeEligible) continue;
    emitBodyLock(3, "case %d:\n", regionIndex);
    emitBodyLock(4, "switch (layer) {\n");
    for (size_t layerIdx = 0; layerIdx < region.layers.size(); layerIdx ++) {
      emitBodyLock(5, "case %zu: layerTaskCount = %zu; break;\n", layerIdx, region.layers[layerIdx].taskCppIds.size());
    }
    emitBodyLock(5, "default: layerTaskCount = 0; break;\n");
    emitBodyLock(4, "}\n");
    emitBodyLock(4, "break;\n");
    regionIndex ++;
  }
  emitBodyLock(3, "default:\n");
  emitBodyLock(4, "layerTaskCount = 0;\n");
  emitBodyLock(4, "break;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "if (layerTaskCount <= 0) continue;\n");
  emitBodyLock(2, "int layerWorkerCount = workerCount;\n");
  emitBodyLock(2, "if (layerWorkerCount > layerTaskCount) layerWorkerCount = layerTaskCount;\n");
  emitBodyLock(2, "if (layerWorkerCount < 1) layerWorkerCount = 1;\n");
  emitBodyLock(2, "if (mtProfileEnabled) {\n");
  emitBodyLock(3, "mtProfileCoarseLayerDispatches ++;\n");
  emitBodyLock(3, "mtProfileCoarseWorkerJobs += layerWorkerCount;\n");
  emitBodyLock(3, "mtProfileCoarseFlagWordCopies += (uint64_t)workerCount * (uint64_t)regionActiveWordSpan;\n");
  emitBodyLock(3, "mtProfileCoarseEstimatedBarrierCount ++;\n");
  emitBodyLock(3, "mtProfileCoarseLayerSizeHist[layerTaskCount <= 1 ? 0 : (layerTaskCount == 2 ? 1 : (layerTaskCount <= 4 ? 2 : (layerTaskCount <= 8 ? 3 : (layerTaskCount <= 15 ? 4 : 5))))] ++;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "if (layerWorkerCount == 1) {\n");
  emitBodyLock(3, "mtRunCoarseLayerWorkerRange(0, regionIndex, layer, 0, layerTaskCount);\n");
  emitBodyLock(2, "} else if (mtWorkerPoolEnabled && mtWorkerPoolThreadCount >= layerWorkerCount) {\n");
  emitBodyLock(3, "{\n");
  emitBodyLock(4, "std::lock_guard<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(4, "mtWorkerPoolJobKind = 1;\n");
  emitBodyLock(4, "mtWorkerPoolCoarseRegionIndex = regionIndex;\n");
  emitBodyLock(4, "mtWorkerPoolCoarseLayerIndex = layer;\n");
  emitBodyLock(4, "mtWorkerPoolCurrentWorkerCount = layerWorkerCount;\n");
  emitBodyLock(4, "mtWorkerPoolDoneCount = 0;\n");
  emitBodyLock(4, "for (int worker = 0; worker < layerWorkerCount; worker ++) {\n");
  emitBodyLock(5, "mtWorkerPoolChunkBegin[worker] = (layerTaskCount * worker) / layerWorkerCount;\n");
  emitBodyLock(5, "mtWorkerPoolChunkEnd[worker] = (layerTaskCount * (worker + 1)) / layerWorkerCount;\n");
  emitBodyLock(4, "}\n");
  emitBodyLock(4, "mtWorkerPoolGeneration ++;\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(3, "mtWorkerPoolCv.notify_all();\n");
  emitBodyLock(3, "{\n");
  emitBodyLock(4, "std::unique_lock<std::mutex> lock(mtWorkerPoolMutex);\n");
  emitBodyLock(4, "mtWorkerPoolDoneCv.wait(lock, [&]() { return mtWorkerPoolDoneCount >= mtWorkerPoolCurrentWorkerCount; });\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "} else {\n");
  emitBodyLock(3, "std::vector<std::thread> workers;\n");
  emitBodyLock(3, "workers.reserve(layerWorkerCount);\n");
  emitBodyLock(3, "for (int worker = 0; worker < layerWorkerCount; worker ++) {\n");
  emitBodyLock(4, "int chunkBegin = (layerTaskCount * worker) / layerWorkerCount;\n");
  emitBodyLock(4, "int chunkEnd = (layerTaskCount * (worker + 1)) / layerWorkerCount;\n");
  emitBodyLock(4, "workers.emplace_back([&, worker, chunkBegin, chunkEnd]() { mtRunCoarseLayerWorkerRange(worker, regionIndex, layer, chunkBegin, chunkEnd); });\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(3, "for (std::thread &worker : workers) worker.join();\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "std::chrono::steady_clock::time_point mtProfileMergeBegin;\n");
  emitBodyLock(2, "if (mtProfileEnabled) mtProfileMergeBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(2, "for (int worker = 0; worker < layerWorkerCount; worker ++) {\n");
  emitBodyLock(3, "for (int word = 0; word < regionActiveWordSpan; word ++) coarseActiveWords[word] |= mtWorkerCoarseFlags[worker][word];\n");
  emitBodyLock(3, "for (const ActivationDeltaEntry &entry : mtWorkerDeltas[worker].entries) {\n");
  emitBodyLock(4, "int localWord = entry.idx - regionBeginActiveWord;\n");
  emitBodyLock(4, "if (localWord >= 0 && localWord < regionActiveWordSpan) coarseActiveWords[localWord] |= (uint%d_t)entry.mask;\n", ACTIVE_WIDTH);
  emitBodyLock(4, "else activeFlags[entry.idx] |= (uint%d_t)entry.mask;\n", ACTIVE_WIDTH);
  emitBodyLock(3, "}\n");
  emitBodyLock(3, "if (mtWorkerDeltas[worker].allActive) {\n");
  emitBodyLock(4, "for (int word = 0; word < regionActiveWordSpan; word ++) coarseActiveWords[word] = (uint%d_t)-1;\n", ACTIVE_WIDTH);
  emitBodyLock(4, "for (int word = 0; word < %d; word ++) activeFlags[word] = (uint%d_t)-1;\n", activeFlagNum, ACTIVE_WIDTH);
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "if (mtProfileEnabled) {\n");
  emitBodyLock(3, "mtProfileCoarseMergeWordScans += (uint64_t)layerWorkerCount * (uint64_t)regionActiveWordSpan;\n");
  emitBodyLock(3, "for (int worker = 0; worker < layerWorkerCount; worker ++) {\n");
  emitBodyLock(4, "mtProfileCoarseActivationDeltaEntries += mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(4, "mtProfileActivationDeltaEntries += mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(4, "if (mtWorkerDeltas[worker].entries.size() > mtProfileActivationDeltaMaxEntriesPerWorker) mtProfileActivationDeltaMaxEntriesPerWorker = mtWorkerDeltas[worker].entries.size();\n");
  emitBodyLock(4, "if (mtWorkerDeltas[worker].allActive) mtProfileActivationDeltaActivateAllCount ++;\n");
  emitBodyLock(4, "mtProfileWorkerTaskCount[(size_t)worker] += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(4, "mtProfilePureTasks += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(4, "for (int cppId : mtProfileLocalTaskIds[worker]) mtProfileTaskExecCount[cppId] ++;\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "if (mtProfileEnabled) mtProfileMergeWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileMergeBegin).count();\n");
  emitBodyLock(2, "if (mtProfileEnabled) {\n");
  emitBodyLock(3, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(4, "mtProfileLocalWorkerTaskCount[worker] = 0;\n");
  emitBodyLock(4, "mtProfileLocalTaskIds[worker].clear();\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(1, "if (mtProfileEnabled && workerCount > 1) mtProfileTrueParallelWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(0, "}\n");
}

int graph::genActivateSeqHelpers(bool buffered) {
    std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCut();
    for (int idx = 0; idx < superId; idx ++) {
      genMtTaskHelper(cppId2Super[idx], buffered, "ActiveBuffer");
    }

    emitFuncDecl(0, "void S%s::subStep0() {\n", name.c_str());
    int indent = 1;
    int nextSubStepIdx = 1;
    std::string nextFuncDef = format("void S%s::subStep%d()", name.c_str(), nextSubStepIdx);
    bool prevActiveWhole = false;
    for (int idx = 0; idx < superId; idx ++) {
      int id;
      uint64_t mask;
      std::tie(id, mask) = setIdxMask(idx);
      int offset = idx % ACTIVE_WIDTH;
      if (offset == 0) {
        if (prevActiveWhole) {
          emitBodyLock(--indent, "}\n");
        }
        prevActiveWhole = true;
        for (int j = 0; j < ACTIVE_WIDTH && idx + j < superId; j ++) {
          if (isAlwaysActive(idx + j)) prevActiveWhole = false;
        }
        if (prevActiveWhole) {
          bool newFile = __emitSrc(indent ++, true, false, nextFuncDef.c_str(), "if(unlikely(activeFlags[%d] != 0)) {\n", id);
          if (newFile) {
            nextFuncDef = format("void S%s::subStep%d()", name.c_str(), ++ nextSubStepIdx);
          }
          emitBodyLock(indent, "uint%d_t oldFlag = activeFlags[%d];\n", ACTIVE_WIDTH, id);
          emitBodyLock(indent, "activeFlags[%d] = 0;\n", id);
          emitBodyLock(indent, "if (mtProfileEnabled) mtProfileActiveWordCount ++;\n");
        } else if (buffered) {
          emitBodyLock(indent, "uint%d_t activeWord%d = activeFlags[%d];\n", ACTIVE_WIDTH, id, id);
        }
      }
      SuperNode* super = cppId2Super[idx];
      std::string flagName = prevActiveWhole ? "oldFlag" : (buffered ? format("activeWord%d", id) : format("activeFlags[%d]", id));
      indent = genNodeStepStart(super, mask, idx, flagName, indent);
      emitBodyLock(indent ++, "{\n");
      if (buffered) {
        emitBodyLock(indent, "ActiveBuffer mtBuffer;\n");
        emitBodyLock(indent, "mtBuffer.clear();\n");
        emitBodyLock(indent, "std::chrono::steady_clock::time_point mtProfileTaskBegin;\n");
        emitBodyLock(indent, "if (mtProfileEnabled) mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
        emitBodyLock(indent, "mtTask%d(%s, mtBuffer);\n", idx, flagName.c_str());
        emitBodyLock(indent, "recordMtProfileTask(%d, %s, mtProfileEnabled ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileTaskBegin).count() : 0);\n",
                     idx, mtTasks[idx].taskKind == "pure_compute" ? "true" : "false");
        emitBodyLock(indent, "mtBuffer.mergeFrom(activeFlags);\n");
      } else {
        emitBodyLock(indent, "std::chrono::steady_clock::time_point mtProfileTaskBegin;\n");
        emitBodyLock(indent, "if (mtProfileEnabled) mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
        emitBodyLock(indent, "mtTask%d(%s);\n", idx, flagName.c_str());
        emitBodyLock(indent, "recordMtProfileTask(%d, %s, mtProfileEnabled ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileTaskBegin).count() : 0);\n",
                     idx, mtTasks[idx].taskKind == "pure_compute" ? "true" : "false");
      }
      emitBodyLock(-- indent, "}\n");
      indent = genNodeStepEnd(super, indent);
    }
    emitBodyLock(--indent, "}\n");
    if (prevActiveWhole) emitBodyLock(--indent, "}\n");

    return nextSubStepIdx - 1;
}

int graph::genActivateMtHelpers() {
    std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCutSelection();
    markMtRepCutLiteRuntimeApplied(mtTasks);
    MtRepCutSemanticPlan semanticPlan = planMtRepCutSemantics(mtTasks);
    MtCoarseRegionPlan coarsePlan = planMtCoarseRegions(mtTasks);
    MtPureBatchPlan batchPlan = semanticPlan.batchPlan;
    std::map<int, int> batchEndByStart;
    for (auto batch : batchPlan.batches) {
      batchEndByStart[batch.first] = batch.second;
    }
    std::map<int, int> coarseRegionIndexByStart;
    std::map<int, MtCoarseRegion> coarseRegionByStart;
    if (globalConfig.MtBatchFormationMode == "coarse") {
      int regionIndex = 0;
      for (const MtCoarseRegion& region : coarsePlan.regions) {
        if (!region.runtimeEligible) continue;
        coarseRegionIndexByStart[region.beginCppId] = regionIndex;
        coarseRegionByStart[region.beginCppId] = region;
        regionIndex ++;
      }
    }
    for (int idx = 0; idx < superId; idx ++) {
      genMtTaskHelper(cppId2Super[idx], true, "ActivationDelta");
      genMtTaskHelper(cppId2Super[idx], false, "ActivationDelta");
    }
    for (int idx = 0; idx < superId; idx ++) {
      if (mtTasks[idx].repcutRuntimeApplied) genMtRepCutLiteTaskHelper(cppId2Super[idx], mtRepCutClonesForSink(semanticPlan, idx), "ActivationDelta");
    }
    genMtTaskRunner(semanticPlan);
    if (globalConfig.MtBatchFormationMode == "coarse") genMtCoarseRegionRunner(semanticPlan, coarsePlan);

    emitFuncDecl(0, "void S%s::subStep0() {\n", name.c_str());
    int indent = 1;
    int nextSubStepIdx = 1;
    std::string nextFuncDef = format("void S%s::subStep%d()", name.c_str(), nextSubStepIdx);
    bool prevActiveWhole = false;
    for (int idx = 0; idx < superId; idx ++) {
      int id;
      uint64_t mask;
      std::tie(id, mask) = setIdxMask(idx);
      int offset = idx % ACTIVE_WIDTH;
      auto coarseIter = coarseRegionIndexByStart.find(idx);
      if (coarseIter != coarseRegionIndexByStart.end()) {
        const MtCoarseRegion& region = coarseRegionByStart[idx];
        if (prevActiveWhole) emitBodyLock(--indent, "}\n");
        prevActiveWhole = false;
        emitBodyLock(indent, "uint%d_t mtCoarseWords%d[%d];\n", ACTIVE_WIDTH, idx, region.activeWordSpan);
        std::string coarseGuard;
        for (int word = 0; word < region.activeWordSpan; word ++) {
          int activeWord = region.beginActiveWord + word;
          emitBodyLock(indent, "mtCoarseWords%d[%d] = activeFlags[%d];\n", idx, word, activeWord);
          emitBodyLock(indent, "activeFlags[%d] = 0;\n", activeWord);
          if (!coarseGuard.empty()) coarseGuard += " | ";
          coarseGuard += format("mtCoarseWords%d[%d]", idx, word);
        }
        emitBodyLock(indent ++, "if(unlikely((%s) != 0)) {\n", coarseGuard.c_str());
        emitBodyLock(indent, "if (mtProfileEnabled) {\n");
        for (int word = 0; word < region.activeWordSpan; word ++) {
          emitBodyLock(indent + 1, "if (mtCoarseWords%d[%d] != 0) mtProfileActiveWordCount ++;\n", idx, word);
        }
        emitBodyLock(indent, "}\n");
        emitBodyLock(indent, "mtRunCoarseRegion(%d, mtCoarseWords%d);\n", coarseIter->second, idx);
        emitBodyLock(--indent, "}\n");
        for (int word = 0; word < region.activeWordSpan; word ++) {
          int activeWord = region.beginActiveWord + word;
          emitBodyLock(indent, "activeFlags[%d] |= mtCoarseWords%d[%d];\n", activeWord, idx, word);
        }
        idx = region.endCppId - 1;
        continue;
      }
      if (offset == 0) {
        if (prevActiveWhole) {
          emitBodyLock(--indent, "}\n");
        }
        prevActiveWhole = true;
        for (int j = 0; j < ACTIVE_WIDTH && idx + j < superId; j ++) {
          if (isAlwaysActive(idx + j)) prevActiveWhole = false;
        }
        if (prevActiveWhole) {
          bool newFile = __emitSrc(indent ++, true, false, nextFuncDef.c_str(), "if(unlikely(activeFlags[%d] != 0)) {\n", id);
          if (newFile) {
            nextFuncDef = format("void S%s::subStep%d()", name.c_str(), ++ nextSubStepIdx);
          }
          emitBodyLock(indent, "uint%d_t oldFlag = activeFlags[%d];\n", ACTIVE_WIDTH, id);
          emitBodyLock(indent, "activeFlags[%d] = 0;\n", id);
          emitBodyLock(indent, "if (mtProfileEnabled) mtProfileActiveWordCount ++;\n");
        } else {
          emitBodyLock(indent, "uint%d_t activeWord%d = activeFlags[%d];\n", ACTIVE_WIDTH, id, id);
        }
      }

      auto batchIter = batchEndByStart.find(idx);
      if (prevActiveWhole && batchIter != batchEndByStart.end()) {
        int batchEnd = batchIter->second;
        if (batchEnd - idx > 1) {
          emitBodyLock(indent, "mtRunPureBatch(%d, %d, oldFlag);\n", idx, batchEnd);
          idx = batchEnd - 1;
          continue;
        }
      }

      SuperNode* super = cppId2Super[idx];
      std::string flagName = prevActiveWhole ? "oldFlag" : format("activeWord%d", id);
      indent = genNodeStepStart(super, mask, idx, flagName, indent);
      emitBodyLock(indent ++, "{\n");
      emitBodyLock(indent, "if (mtProfileEnabled) {\n");
      emitBodyLock(indent + 1, "if (mtProfileEffectiveWorkerCountHist.size() <= 1) mtProfileEffectiveWorkerCountHist.resize(2, 0);\n");
      emitBodyLock(indent + 1, "mtProfileEffectiveWorkerCountHist[1] ++;\n");
      if (!prevActiveWhole) {
        emitBodyLock(indent + 1, "mtProfileRejectNotActiveWhole ++;\n");
      } else if (isAlwaysActive(idx)) {
        emitBodyLock(indent + 1, "mtProfileRejectAlwaysActiveTask ++;\n");
      } else if (mtTasks[idx].taskKind != "pure_compute") {
        emitBodyLock(indent + 1, "mtProfileRejectSerialTask ++;\n");
      } else if (mtTaskHasSameActiveWordHazard(mtTasks, idx, globalConfig.MtRepCutLiteMode == "on")) {
        emitBodyLock(indent + 1, "mtProfileRejectSameActiveWordHazard ++;\n");
      } else {
        emitBodyLock(indent + 1, "mtProfileRejectBelowMinBatch ++;\n");
      }
      emitBodyLock(indent, "}\n");
      emitBodyLock(indent, "if (mtProfileEnabled) {\n");
      emitBodyLock(indent + 1, "std::chrono::steady_clock::time_point mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
      emitBodyLock(indent + 1, "mtTask%d(%s);\n", idx, flagName.c_str());
      emitBodyLock(indent + 1, "recordMtProfileTask(%d, %s, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileTaskBegin).count());\n",
                   idx, mtTasks[idx].taskKind == "pure_compute" ? "true" : "false");
      emitBodyLock(indent, "} else {\n");
      emitBodyLock(indent + 1, "mtTask%d(%s);\n", idx, flagName.c_str());
      emitBodyLock(indent, "}\n");
      emitBodyLock(-- indent, "}\n");
      indent = genNodeStepEnd(super, indent);
    }
    emitBodyLock(--indent, "}\n");
    if (prevActiveWhole) emitBodyLock(--indent, "}\n");

    return nextSubStepIdx - 1;
}

void graph::genResetDef(SuperNode* super, bool isUIntReset, bool buffered, int resetId, int indent) {
  std::string activeSinkType = globalConfig.MtHelperMode == "mt" ? "ActivationDelta" : "ActiveBuffer";
  if (buffered) emitBodyLock(indent ++, "void S%s::subReset%d(%s &nextActive){ // %s reset\n", name.c_str(), resetId, activeSinkType.c_str(), isUIntReset ? "uint" : "async");
  else emitBodyLock(indent ++, "void S%s::subReset%d(){ // %s reset\n", name.c_str(), resetId, isUIntReset ? "uint" : "async");
  std::string resetName = super->resetNode->type == NODE_REG_SRC ? RESET_NAME(super->resetNode).c_str() : super->resetNode->name.c_str();
  emitBodyLock(indent ++, "if(unlikely(%s)) {\n", resetName.c_str());
  std::set<int> allNext;
  for (size_t i = 0; i < super->member.size(); i ++) {
    Node* node = super->member[i];
    if (node->type == NODE_REG_RESET) node = node->getResetSrc();
    for (Node* next : node->next) {
      if (next->super->cppId >= 0) allNext.insert(next->super->cppId);
    }
  }

  if (allNext.size() > 100) {
    if (buffered) emitBodyLock(indent, "nextActive.activateAll();\n");
    else emitBodyLock(indent, "activateAll();\n");
  }
  else {
    std::map<uint64_t, ActiveType> bitMapInfo;
    activeSet2bitMap(allNext, bitMapInfo, -1);
    for (auto iter : bitMapInfo) {
      emitBodyLock(indent, "%s // %s\n", updateActiveStr(iter.first, ACTIVE_MASK(iter.second), buffered ? "nextActive" : "").c_str(), ACTIVE_COMMENT(iter.second).c_str());
    }
  }
  emitBodyLock(-- indent, "}\n");
  for (InstInfo inst : super->insts) {
    switch (inst.infoType) {
      case SUPER_INFO_IF:
        emitBodyLock(indent ++, "%s\n", inst.inst.c_str());
        break;
      case SUPER_INFO_DEDENT:
        emitBodyLock(--indent, "%s\n", inst.inst.c_str());
        break;
      case SUPER_INFO_ELSE:
      case SUPER_INFO_STR:
        emitBodyLock(indent, "%s\n", inst.inst.c_str());
        break;
      default:
        break;
    }
  }
  emitBodyLock(-- indent, "}\n");
}

void graph::genResetActivation(SuperNode* super, bool isUIntReset, int indent, int resetId) {
  emitBodyLock(indent, "subReset%d();\n", resetId);
}

void graph::genResetAll() {
  std::vector<SuperNode*> resetSuper;
  for (SuperNode* super : allReset) {
    if (super->resetNode->status == CONSTANT_NODE) {
      Assert(mpz_sgn(super->resetNode->computeInfo->consVal) == 0, "reset %s is always true", super->resetNode->name.c_str());
      continue;
    }
    if (super2ResetId.find(super->resetNode) != super2ResetId.end()) {
      super2ResetId[super->resetNode] = std::make_pair(-1, -1);
    }
    int resetId = resetFuncNum ++;
    bool isUIntReset = super->superType == SUPER_UINT_RESET;
    if (isUIntReset) super2ResetId[super->resetNode].first = resetId;
    else super2ResetId[super->resetNode].second = resetId;
    genResetDef(super, isUIntReset, false, resetId, 0);
    if (globalConfig.MtHelperMode == "buffered-seq" || globalConfig.MtHelperMode == "mt") {
      genResetDef(super, isUIntReset, true, resetId, 0);
    }
    resetSuper.push_back(super);
  }

  emitFuncDecl(0, "void S%s::resetAll(){\n", name.c_str());
  for (size_t i = 0; i < resetSuper.size(); i ++) {
    if (resetSuper[i]->superType == SUPER_ASYNC_RESET) continue;
    genResetActivation(resetSuper[i], true, 1, i);
  }
  emitBodyLock(0, "}\n");
}

void graph::genStep(int subStepIdxMax) {
  emitFuncDecl(0, "void S%s::step() {\n", name.c_str());
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileStepBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileStepBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "resetAll();\n");
  for (SuperNode* super : sortedSuper) {
    for (Node* member : super->member) {
      if (member->isReset() && member->type == NODE_REG_SRC) {
        emitBodyLock(1, "%s = %s;\n", RESET_NAME(member).c_str(), member->name.c_str());
      }
    }
  }
  for (int i = 0; i <= subStepIdxMax; i ++) {
    emitBodyLock(1, "subStep%d();\n", i);
  }

  emitBodyLock(1, "cycles ++;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileTotalStepNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileStepBegin).count();\n");
  emitBodyLock(0, "}\n");
}

bool SuperNode::instsEmpty() {
  return insts.size() == 0;
}

bool graph::__emitSrc(int indent, bool canNewFile, bool alreadyEndFunc, const char *nextFuncDef, const char *fmt, ...) {
  bool newFile = false;
  if (srcFp == NULL || (srcFileBytes > (globalConfig.cppMaxSizeKB * 1024) && canNewFile)) {
    if (srcFp != NULL) {
      if (!alreadyEndFunc) fprintf(srcFp, "}"); // the end of the current function
      fclose(srcFp);
    }
    srcFp = std::fopen(format("%s%d.cpp", (globalConfig.OutputDir + "/" + name).c_str(), srcFileIdx).c_str(), "w");
    srcFileIdx ++;
    assert(srcFp != NULL);
    srcFileBytes = fprintf(srcFp, "#include \"%s.h\"\n", name.c_str());
    if (nextFuncDef != NULL) {
      srcFileBytes += fprintf(srcFp, "%s {\n", nextFuncDef);
    }
    newFile = true;
  }
  for (int i = 0; i < indent; i ++) fprintf(srcFp, "  ");
  va_list args;
  va_start(args, fmt);
  int bytes = vfprintf(srcFp, fmt, args);
  assert(bytes > 0);
  va_end(args);
  srcFileBytes += bytes;
  return newFile;
}

void graph::emitPrintf() {
  emitFuncDecl(0, "void gprintf(const char *fmt, ...) {\n");
  emitBodyLock(0,
  "  FILE *fp = stderr;\n"
  "  va_list args;\n"
  "  va_start(args, fmt);\n"
  "  int fmt_idx = 0;\n"
  "  while (true) {\n"
  "    char c = fmt[fmt_idx ++];\n"
  "    switch (c) {\n"
  "      case '%%': break;\n"
  "      case 0: return;\n"
  "      default: fputc(c, fp); continue;\n"
  "    }\n"
  "\n"
  "    uint64_t lval = 0;\n"
  "    int bits = va_arg(args, uint32_t);\n"
  "    if      (bits <= 32) { lval = va_arg(args, uint32_t); }\n"
  "    else if (bits <= 64) { lval = va_arg(args, uint64_t); }\n"
  "    else                 { assert(0); }\n"
  "\n"
  "    c = fmt[fmt_idx ++];\n"
  "    switch (c) {\n"
  "      case 'd': fprintf(fp, \"%%ld\", lval); break;\n"
  "      case 'c': fputc(lval & 0xff, fp); break;\n"
  "      case 'x': fprintf(fp, \"%%lx\", lval); break;\n"
  "      default: assert(0);\n"
  "    }\n"
  "  }\n"
  "}\n"
  );
}

void graph::cppEmitter() {
  for (SuperNode* super : sortedSuper) {
    if (!super->instsEmpty() || super->superType == SUPER_EXTMOD || super->superType == SUPER_ASYNC_RESET) {
      super->cppId = superId ++;
      cppId2Super[super->cppId] = super;
      if (super->superType == SUPER_EXTMOD) {
        alwaysActive.insert(super->cppId);
      }
#if 0
      if (super->member.size() == 1) {
        alwaysActive.insert(super->cppId);
        printf("alwaysActive %d\n", super->cppId);
      }
#endif
    }
  }
  activeFlagNum = (superId + ACTIVE_WIDTH - 1) / ACTIVE_WIDTH;
  // avoid buffer overflow when accessing the last elements as uint64_t
  activeFlagNum = ROUNDUP(activeFlagNum, 8);

  for (SuperNode* super : sortedSuper) {
    for (Node* member : super->member) {
      if (member->status == VALID_NODE) {
        member->updateActivate();
        member->updateNeedActivate(alwaysActive);
      }
    }
  }
  if (globalConfig.DumpMtScheduleJson) dumpMtScheduleJson();
  if (globalConfig.DumpMtRepCutLiteReport || globalConfig.MtRepCutLiteMode == "on") dumpMtRepCutLiteReport();
  if (globalConfig.DumpMtCoarseRegionReport || globalConfig.MtBatchFormationMode == "coarse") dumpMtCoarseRegionReport();

  srcFp = NULL;
  srcFileIdx = 0;

  FILE* header = genHeaderStart();
#ifdef DIFFTEST_PER_SIG
  sigFile = fopen((globalConfig.OutputDir + "/" + name + "_sigs.txt").c_str(), "w");
#endif

  /* class start*/
  bool useMtHelpers = globalConfig.MtHelperMode == "mt";
  bool useSeqHelpers = globalConfig.MtHelperMode == "seq" ||
                       globalConfig.MtHelperMode == "buffered-seq";
  bool useBufferedHelpers = globalConfig.MtHelperMode == "buffered-seq" || useMtHelpers;
  bool useHelperTasks = useSeqHelpers || useMtHelpers;
  bool useCoarseMt = useMtHelpers && globalConfig.MtBatchFormationMode == "coarse";
  std::map<int, MtTaskInfo> mtRepCutHeaderTasks;
  MtCoarseProfileFacts mtCoarseProfileFacts;
  if (useMtHelpers) {
    mtRepCutHeaderTasks = buildMtTaskInfoMapWithRepCutSelection();
    markMtRepCutLiteRuntimeApplied(mtRepCutHeaderTasks);
    if (useCoarseMt) {
      mtCoarseProfileFacts = mtComputeCoarseProfileFacts(planMtCoarseRegions(mtRepCutHeaderTasks));
    }
  }
  if (globalConfig.MtHelperMode == "buffered-seq") emitActiveBufferDef(header, activeFlagNum);
  if (useMtHelpers) emitActivationDeltaDef(header, activeFlagNum);

  fprintf(header, "class S%s {\npublic:\n", name.c_str());
  fprintf(header, "uint64_t cycles;\n");
  fprintf(header, "uint64_t LOG_START, LOG_END;\n");
  fprintf(header, "uint%d_t activeFlags[%d];\n", ACTIVE_WIDTH, activeFlagNum); // or super.size() if id == idx
  fprintf(header, "bool mtProfileEnabled;\n");
  fprintf(header, "const char *mtProfileHelperMode;\n");
  fprintf(header, "int mtConfiguredWorkerCount;\n");
  fprintf(header, "int mtMinBatchTasks;\n");
  fprintf(header, "int mtProfileConfiguredWorkerCount;\n");
  fprintf(header, "int mtProfileMaxWorkerCount;\n");
  fprintf(header, "uint64_t mtProfileActiveWordCount;\n");
  fprintf(header, "uint64_t mtProfileSerialTasks;\n");
  fprintf(header, "uint64_t mtProfilePureTasks;\n");
  fprintf(header, "uint64_t mtProfilePureBatchCount;\n");
  fprintf(header, "uint64_t mtProfileTrueParallelBatchCount;\n");
  fprintf(header, "uint64_t mtProfileSkippedFakeParallelBatchCount;\n");
  fprintf(header, "uint64_t mtProfileSerialFastTaskCount;\n");
  fprintf(header, "uint64_t mtProfileActivationDeltaEntries;\n");
  fprintf(header, "uint64_t mtProfileActivationDeltaMaxEntriesPerWorker;\n");
  fprintf(header, "uint64_t mtProfileActivationDeltaActivateAllCount;\n");
  fprintf(header, "uint64_t mtProfileRejectNotActiveWhole;\n");
  fprintf(header, "uint64_t mtProfileRejectAlwaysActiveTask;\n");
  fprintf(header, "uint64_t mtProfileRejectSerialTask;\n");
  fprintf(header, "uint64_t mtProfileRejectDependencyEdge;\n");
  fprintf(header, "uint64_t mtProfileRejectSameActiveWordHazard;\n");
  fprintf(header, "uint64_t mtProfileRejectBelowMinBatch;\n");
  fprintf(header, "uint64_t mtProfileRejectConfiguredSingleWorker;\n");
  fprintf(header, "uint64_t mtProfileBatchMemberNodeCount;\n");
  fprintf(header, "uint64_t mtProfileSameActiveWordForwardEdges;\n");
  fprintf(header, "uint64_t mtProfileCrossBatchActivationFanout;\n");
  fprintf(header, "uint64_t mtProfileBatchWallNs;\n");
  fprintf(header, "uint64_t mtProfileTrueParallelWallNs;\n");
  fprintf(header, "uint64_t mtProfileSerialWallNs;\n");
  fprintf(header, "uint64_t mtProfileMergeWallNs;\n");
  fprintf(header, "uint64_t mtProfileTotalStepNs;\n");
  if (useCoarseMt) {
    fprintf(header, "uint64_t mtProfileCoarseStaticRuntimeEligibleRegions;\n");
    fprintf(header, "uint64_t mtProfileCoarseStaticLayerCount;\n");
    fprintf(header, "uint64_t mtProfileCoarseStaticMaxRegionLayerCount;\n");
    fprintf(header, "uint64_t mtProfileCoarseStaticMTaskCount;\n");
    fprintf(header, "uint64_t mtProfileCoarseRegionInvocations;\n");
    fprintf(header, "uint64_t mtProfileCoarseLayerDispatches;\n");
    fprintf(header, "uint64_t mtProfileCoarseMTaskDispatches;\n");
    fprintf(header, "uint64_t mtProfileCoarseWorkerJobs;\n");
    fprintf(header, "uint64_t mtProfileCoarseFlagWordCopies;\n");
    fprintf(header, "uint64_t mtProfileCoarseMergeWordScans;\n");
    fprintf(header, "uint64_t mtProfileCoarseActivationDeltaEntries;\n");
    fprintf(header, "uint64_t mtProfileCoarseEstimatedBarrierCount;\n");
    fprintf(header, "uint64_t mtProfileCoarseLayerSizeHist[6];\n");
    fprintf(header, "uint64_t mtProfileCoarseRegionLayerCountHist[6];\n");
  }
  fprintf(header, "uint64_t mtProfileTaskExecCount[%d];\n", superId);
    fprintf(header, "std::vector<uint64_t> mtProfileWorkerTaskCount;\n");
    fprintf(header, "uint64_t mtProfileBatchSizeHist[6];\n");
    fprintf(header, "std::vector<uint64_t> mtProfileEffectiveWorkerCountHist;\n");
    if (useCoarseMt) {
      fprintf(header, "std::vector<uint64_t> mtProfileLocalActivationDeltaEntries;\n");
      fprintf(header, "std::vector<uint64_t> mtProfileLocalActivationDeltaMaxEntries;\n");
    }
  if (useMtHelpers) {
    fprintf(header, "std::vector<ActivationDelta> mtWorkerDeltas;\n");
    fprintf(header, "std::vector<uint%d_t> mtWorkerFlags;\n", ACTIVE_WIDTH);
    if (useCoarseMt) {
      fprintf(header, "std::vector<std::vector<uint%d_t>> mtWorkerCoarseFlags;\n", ACTIVE_WIDTH);
      fprintf(header, "int mtWorkerPoolJobKind;\n");
      fprintf(header, "int mtWorkerPoolCoarseRegionIndex;\n");
      fprintf(header, "int mtWorkerPoolCoarseLayerIndex;\n");
    }
    fprintf(header, "bool mtWorkerPoolEnabled;\n");
    fprintf(header, "int mtWorkerPoolThreadCount;\n");
    fprintf(header, "std::vector<std::thread> mtWorkerPoolThreads;\n");
    fprintf(header, "std::mutex mtWorkerPoolMutex;\n");
    fprintf(header, "std::condition_variable mtWorkerPoolCv;\n");
    fprintf(header, "std::condition_variable mtWorkerPoolDoneCv;\n");
    fprintf(header, "uint64_t mtWorkerPoolGeneration;\n");
    fprintf(header, "bool mtWorkerPoolStop;\n");
    fprintf(header, "int mtWorkerPoolDoneCount;\n");
    fprintf(header, "std::vector<int> mtWorkerPoolChunkBegin;\n");
    fprintf(header, "std::vector<int> mtWorkerPoolChunkEnd;\n");
    fprintf(header, "int mtWorkerPoolCurrentWorkerCount;\n");
    fprintf(header, "std::vector<std::vector<int>> mtProfileLocalTaskIds;\n");
    fprintf(header, "std::vector<uint64_t> mtProfileLocalWorkerTaskCount;\n");
  }
#ifdef PERF
  fprintf(header, "size_t activeTimes[%d];\n", superId);
#if ENABLE_ACTIVATOR
  fprintf(header, "std::map<int, int>activator[%d];\n", superId);
#endif
  fprintf(header, "size_t validActive[%d];\n", superId);
  fprintf(header, "size_t nodeNum[%d];\n", superId);
#endif
  emitPrintf();
  /* constrcutor */
  emitFuncDecl(0, "S%s::S%s() {\n", name.c_str(), name.c_str());
  emitBodyLock(1, "cycles = 0;\n");
  emitBodyLock(1, "LOG_START = 1;\n");
  emitBodyLock(1, "LOG_END = 0;\n");
  emitBodyLock(1, "initMtProfile();\n");
  if (useMtHelpers) emitBodyLock(1, "startMtWorkerPool();\n");
  emitBodyLock(1, "init();\n");
  emitBodyLock(0, "}\n");

  /* initialization */
  emitFuncDecl(0, "void S%s::init() {\n", name.c_str());
  emitBodyLock(1, "activateAll();\n");
#ifdef PERF
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) activeTimes[i] = 0;\n", superId);
  #if ENABLE_ACTIVATOR
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) activator[i] = std::map<int, int>();\n", superId);
  #endif
  for (SuperNode* super : sortedSuper) {
    if (super->cppId >= 0) {
      size_t num = 0;
      for (Node* member : super->member) {
        if (member->anyNextActive()) num ++;
      }
      emitBodyLock(1, "nodeNum[%d] = %ld; // memberNum=%ld\n", super->cppId, num, super->member.size());
    }
  }
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) validActive[i] = 0;\n", superId);
#endif
  emitBodyLock(0, "#ifdef RANDOMIZE_INIT\n"
               "  srand((unsigned int)time(NULL));\n"
               "  for (uint32_t *p = &_var_start; p != &_var_end; p ++) {\n"
               "    *p = rand();\n"
               "  }\n"
               "// mask out the bits out of the width range\n");

  // header: node definition; src: node evaluation
  fprintf(header, "uint32_t _var_start;\n");
  for (SuperNode* super : sortedSuper) {
    // std::string insts;
    if (super->superType == SUPER_VALID || super->superType == SUPER_ASYNC_RESET) {
      for (Node* n : super->member) genNodeDef(header, n);
    }
    if (super->superType == SUPER_EXTMOD) {
      for (size_t i = 1; i < super->member.size(); i ++) genNodeDef(header, super->member[i]);
    }
  }
  /* memory definition */
  for (Node* mem : memory) genNodeDef(header, mem);
  fprintf(header, "uint32_t _var_end;\n");

  emitBodyLock(0, "// initialize registers with reset value 0 to overwrite the rand() results\n" );
  emitBodyLock(1, "memset(&_var_start, 0, &_var_end - &_var_start);\n");

  emitBodyLock(0, "#else\n" // RANDOMIZE_INIT
               "  memset(&_var_start, 0, &_var_end - &_var_start);\n"
               "#endif\n");

  fprintf(header, "S%s();\n", name.c_str());
  fprintf(header, "~S%s();\n", name.c_str());
  fprintf(header, "void init();\n");
  fprintf(header, "void initMtProfile();\n");
  fprintf(header, "void dumpMtProfile();\n");
  fprintf(header, "void recordMtProfileTask(int cppId, bool pureTask, uint64_t elapsedNs);\n");
  fprintf(header, "void recordMtProfileWorkerTask(int worker);\n");

  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::initMtProfile() {\n", name.c_str());
  emitBodyLock(1, "const char *profileEnv = getenv(\"GSIM_MT_PROFILE\");\n");
  emitBodyLock(1, "mtProfileEnabled = profileEnv != nullptr && profileEnv[0] != '\\0' && profileEnv[0] != '0';\n");
  emitBodyLock(1, "mtProfileHelperMode = \"%s\";\n", globalConfig.MtHelperMode.c_str());
  emitBodyLock(1, "const char *threadsEnv = getenv(\"GSIM_THREADS\");\n");
  emitBodyLock(1, "mtConfiguredWorkerCount = threadsEnv == nullptr ? 1 : atoi(threadsEnv);\n");
  emitBodyLock(1, "if (mtConfiguredWorkerCount < 1) mtConfiguredWorkerCount = 1;\n");
  emitBodyLock(1, "int mtMinBatchTasks = %d;\n", globalConfig.MtBatchFormationMode == "active-frequency" ? 2 : 16);
  emitBodyLock(1, "const char *minBatchEnv = getenv(\"GSIM_MT_MIN_BATCH_TASKS\");\n");
  emitBodyLock(1, "if (minBatchEnv != nullptr) mtMinBatchTasks = atoi(minBatchEnv);\n");
  emitBodyLock(1, "if (mtMinBatchTasks < 1) mtMinBatchTasks = 1;\n");
  emitBodyLock(1, "this->mtMinBatchTasks = mtMinBatchTasks;\n");
  emitBodyLock(1, "mtProfileConfiguredWorkerCount = mtConfiguredWorkerCount;\n");
  emitBodyLock(1, "mtProfileMaxWorkerCount = 1;\n");
  emitBodyLock(1, "mtProfileActiveWordCount = 0;\n");
  emitBodyLock(1, "mtProfileSerialTasks = 0;\n");
  emitBodyLock(1, "mtProfilePureTasks = 0;\n");
  emitBodyLock(1, "mtProfilePureBatchCount = 0;\n");
  emitBodyLock(1, "mtProfileTrueParallelBatchCount = 0;\n");
  emitBodyLock(1, "mtProfileSkippedFakeParallelBatchCount = 0;\n");
  emitBodyLock(1, "mtProfileSerialFastTaskCount = 0;\n");
  emitBodyLock(1, "mtProfileActivationDeltaEntries = 0;\n");
  emitBodyLock(1, "mtProfileActivationDeltaMaxEntriesPerWorker = 0;\n");
  emitBodyLock(1, "mtProfileActivationDeltaActivateAllCount = 0;\n");
  emitBodyLock(1, "mtProfileRejectNotActiveWhole = 0;\n");
  emitBodyLock(1, "mtProfileRejectAlwaysActiveTask = 0;\n");
  emitBodyLock(1, "mtProfileRejectSerialTask = 0;\n");
  emitBodyLock(1, "mtProfileRejectDependencyEdge = 0;\n");
  emitBodyLock(1, "mtProfileRejectSameActiveWordHazard = 0;\n");
  emitBodyLock(1, "mtProfileRejectBelowMinBatch = 0;\n");
  emitBodyLock(1, "mtProfileRejectConfiguredSingleWorker = 0;\n");
  emitBodyLock(1, "mtProfileBatchMemberNodeCount = 0;\n");
  emitBodyLock(1, "mtProfileSameActiveWordForwardEdges = 0;\n");
  emitBodyLock(1, "mtProfileCrossBatchActivationFanout = 0;\n");
  emitBodyLock(1, "mtProfileBatchWallNs = 0;\n");
  emitBodyLock(1, "mtProfileTrueParallelWallNs = 0;\n");
  emitBodyLock(1, "mtProfileSerialWallNs = 0;\n");
  emitBodyLock(1, "mtProfileMergeWallNs = 0;\n");
  emitBodyLock(1, "mtProfileTotalStepNs = 0;\n");
  if (useCoarseMt) {
    emitBodyLock(1, "mtProfileCoarseStaticRuntimeEligibleRegions = %d;\n", mtCoarseProfileFacts.runtimeEligibleRegionCount);
    emitBodyLock(1, "mtProfileCoarseStaticLayerCount = %d;\n", mtCoarseProfileFacts.runtimeLayerCount);
    emitBodyLock(1, "mtProfileCoarseStaticMaxRegionLayerCount = %d;\n", mtCoarseProfileFacts.maxRegionLayerCount);
    emitBodyLock(1, "mtProfileCoarseStaticMTaskCount = %d;\n", mtCoarseProfileFacts.runtimeMTaskCount);
    emitBodyLock(1, "mtProfileCoarseRegionInvocations = 0;\n");
    emitBodyLock(1, "mtProfileCoarseLayerDispatches = 0;\n");
    emitBodyLock(1, "mtProfileCoarseMTaskDispatches = 0;\n");
    emitBodyLock(1, "mtProfileCoarseWorkerJobs = 0;\n");
    emitBodyLock(1, "mtProfileCoarseFlagWordCopies = 0;\n");
    emitBodyLock(1, "mtProfileCoarseMergeWordScans = 0;\n");
    emitBodyLock(1, "mtProfileCoarseActivationDeltaEntries = 0;\n");
    emitBodyLock(1, "mtProfileCoarseEstimatedBarrierCount = 0;\n");
    emitBodyLock(1, "for (int i = 0; i < 6; i ++) mtProfileCoarseLayerSizeHist[i] = 0;\n");
    for (int i = 0; i < 6; i ++) {
      emitBodyLock(1, "mtProfileCoarseRegionLayerCountHist[%d] = %d;\n", i, mtCoarseProfileFacts.regionLayerCountHist[i]);
    }
  }
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) mtProfileTaskExecCount[i] = 0;\n", superId);
  emitBodyLock(1, "for (int i = 0; i < 6; i ++) mtProfileBatchSizeHist[i] = 0;\n");
  emitBodyLock(1, "mtProfileWorkerTaskCount.assign((size_t)mtProfileConfiguredWorkerCount, 0);\n");
  emitBodyLock(1, "mtProfileEffectiveWorkerCountHist.assign((size_t)mtProfileConfiguredWorkerCount + 1, 0);\n");
  if (useMtHelpers) {
    emitBodyLock(1, "const char *workerPoolEnv = getenv(\"GSIM_MT_WORKER_POOL\");\n");
    emitBodyLock(1, "mtWorkerPoolEnabled = workerPoolEnv != nullptr && workerPoolEnv[0] != '\\0' && workerPoolEnv[0] != '0';\n");
    emitBodyLock(1, "mtWorkerPoolThreadCount = 0;\n");
    emitBodyLock(1, "mtWorkerPoolGeneration = 0;\n");
    emitBodyLock(1, "mtWorkerPoolStop = false;\n");
    emitBodyLock(1, "mtWorkerPoolDoneCount = 0;\n");
    emitBodyLock(1, "mtWorkerPoolCurrentWorkerCount = 0;\n");
    if (useCoarseMt) {
      emitBodyLock(1, "mtWorkerPoolJobKind = 0;\n");
      emitBodyLock(1, "mtWorkerPoolCoarseRegionIndex = -1;\n");
      emitBodyLock(1, "mtWorkerPoolCoarseLayerIndex = -1;\n");
    }
  }
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "S%s::~S%s() {\n", name.c_str(), name.c_str());
  if (useMtHelpers) emitBodyLock(1, "stopMtWorkerPool();\n");
  emitBodyLock(1, "dumpMtProfile();\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::recordMtProfileTask(int cppId, bool pureTask, uint64_t elapsedNs) {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  emitBodyLock(1, "if (cppId >= 0 && cppId < %d) mtProfileTaskExecCount[cppId] ++;\n", superId);
  emitBodyLock(1, "if (pureTask) mtProfilePureTasks ++;\n");
  emitBodyLock(1, "else mtProfileSerialTasks ++;\n");
  emitBodyLock(1, "mtProfileSerialFastTaskCount ++;\n");
  emitBodyLock(1, "mtProfileSerialWallNs += elapsedNs;\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::recordMtProfileWorkerTask(int worker) {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  emitBodyLock(1, "if (worker >= 0 && (size_t)worker < mtProfileWorkerTaskCount.size()) mtProfileWorkerTaskCount[(size_t)worker] ++;\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::dumpMtProfile() {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  if (useMtHelpers) {
    emitBodyLock(1, "fprintf(stderr, \"[mt-profile] helper_mode=%%s worker_count=%%d worker_pool=%%d min_batch_tasks=%%d max_worker_count=%%d cycles=%%lu active_word_count=%%lu serial_tasks=%%lu pure_tasks=%%lu pure_batch_count=%%lu true_parallel_batch_count=%%lu skipped_fake_parallel_batch_count=%%lu serial_fast_task_count=%%lu batch_wall_ns=%%lu true_parallel_wall_ns=%%lu serial_wall_ns=%%lu merge_wall_ns=%%lu total_step_ns=%%lu\\n\", mtProfileHelperMode, mtProfileConfiguredWorkerCount, mtWorkerPoolEnabled ? 1 : 0, mtMinBatchTasks, mtProfileMaxWorkerCount, cycles, mtProfileActiveWordCount, mtProfileSerialTasks, mtProfilePureTasks, mtProfilePureBatchCount, mtProfileTrueParallelBatchCount, mtProfileSkippedFakeParallelBatchCount, mtProfileSerialFastTaskCount, mtProfileBatchWallNs, mtProfileTrueParallelWallNs, mtProfileSerialWallNs, mtProfileMergeWallNs, mtProfileTotalStepNs);\n");
  } else {
    emitBodyLock(1, "fprintf(stderr, \"[mt-profile] helper_mode=%%s worker_count=%%d min_batch_tasks=%%d max_worker_count=%%d cycles=%%lu active_word_count=%%lu serial_tasks=%%lu pure_tasks=%%lu pure_batch_count=%%lu true_parallel_batch_count=%%lu skipped_fake_parallel_batch_count=%%lu serial_fast_task_count=%%lu batch_wall_ns=%%lu true_parallel_wall_ns=%%lu serial_wall_ns=%%lu merge_wall_ns=%%lu total_step_ns=%%lu\\n\", mtProfileHelperMode, mtProfileConfiguredWorkerCount, mtMinBatchTasks, mtProfileMaxWorkerCount, cycles, mtProfileActiveWordCount, mtProfileSerialTasks, mtProfilePureTasks, mtProfilePureBatchCount, mtProfileTrueParallelBatchCount, mtProfileSkippedFakeParallelBatchCount, mtProfileSerialFastTaskCount, mtProfileBatchWallNs, mtProfileTrueParallelWallNs, mtProfileSerialWallNs, mtProfileMergeWallNs, mtProfileTotalStepNs);\n");
  }
  if (useCoarseMt) {
    emitBodyLock(1, "fprintf(stderr, \"[mt-profile] coarse_dispatch coarse_runtime=%%s static_runtime_eligible_regions=%%lu static_layer_count=%%lu max_region_layer_count=%%lu static_mtask_count=%%lu region_invocations=%%lu layer_dispatches=%%lu mtask_dispatches=%%lu worker_jobs=%%lu flag_word_copies=%%lu merge_word_scans=%%lu activation_delta_entries=%%lu estimated_barriers=%%lu\\n\", \"%s\", mtProfileCoarseStaticRuntimeEligibleRegions, mtProfileCoarseStaticLayerCount, mtProfileCoarseStaticMaxRegionLayerCount, mtProfileCoarseStaticMTaskCount, mtProfileCoarseRegionInvocations, mtProfileCoarseLayerDispatches, mtProfileCoarseMTaskDispatches, mtProfileCoarseWorkerJobs, mtProfileCoarseFlagWordCopies, mtProfileCoarseMergeWordScans, mtProfileCoarseActivationDeltaEntries, mtProfileCoarseEstimatedBarrierCount);\n", globalConfig.MtCoarseRuntimeMode.c_str());
    emitBodyLock(1, "fprintf(stderr, \"[mt-profile] coarse_layer_size_hist=%%lu,%%lu,%%lu,%%lu,%%lu,%%lu static=%%d,%%d,%%d,%%d,%%d,%%d labels=1,2,3-4,5-8,9-15,16+\\n\", mtProfileCoarseLayerSizeHist[0], mtProfileCoarseLayerSizeHist[1], mtProfileCoarseLayerSizeHist[2], mtProfileCoarseLayerSizeHist[3], mtProfileCoarseLayerSizeHist[4], mtProfileCoarseLayerSizeHist[5], %d, %d, %d, %d, %d, %d);\n",
                 mtCoarseProfileFacts.layerSizeHist[0], mtCoarseProfileFacts.layerSizeHist[1], mtCoarseProfileFacts.layerSizeHist[2],
                 mtCoarseProfileFacts.layerSizeHist[3], mtCoarseProfileFacts.layerSizeHist[4], mtCoarseProfileFacts.layerSizeHist[5]);
    emitBodyLock(1, "fprintf(stderr, \"[mt-profile] coarse_region_layer_count_hist=%%lu,%%lu,%%lu,%%lu,%%lu,%%lu labels=1,2,3-4,5-8,9-15,16+\\n\", mtProfileCoarseRegionLayerCountHist[0], mtProfileCoarseRegionLayerCountHist[1], mtProfileCoarseRegionLayerCountHist[2], mtProfileCoarseRegionLayerCountHist[3], mtProfileCoarseRegionLayerCountHist[4], mtProfileCoarseRegionLayerCountHist[5]);\n");
  }
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] activation_delta entries=%%lu max_entries_per_worker=%%lu activate_all_count=%%lu\\n\", mtProfileActivationDeltaEntries, mtProfileActivationDeltaMaxEntriesPerWorker, mtProfileActivationDeltaActivateAllCount);\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] rejection_reasons not_active_whole=%%lu always_active_task=%%lu serial_task=%%lu dependency_edge=%%lu same_active_word_hazard=%%lu below_min_batch=%%lu configured_single_worker=%%lu\\n\", mtProfileRejectNotActiveWhole, mtProfileRejectAlwaysActiveTask, mtProfileRejectSerialTask, mtProfileRejectDependencyEdge, mtProfileRejectSameActiveWordHazard, mtProfileRejectBelowMinBatch, mtProfileRejectConfiguredSingleWorker);\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] batch_size_hist=%%lu,%%lu,%%lu,%%lu,%%lu,%%lu labels=1,2,3-4,5-8,9-15,16+\\n\", mtProfileBatchSizeHist[0], mtProfileBatchSizeHist[1], mtProfileBatchSizeHist[2], mtProfileBatchSizeHist[3], mtProfileBatchSizeHist[4], mtProfileBatchSizeHist[5]);\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] effective_worker_count_hist=\");\n");
  emitBodyLock(1, "for (size_t i = 0; i < mtProfileEffectiveWorkerCountHist.size(); i ++) fprintf(stderr, \"%%s%%zu:%%lu\", i == 0 ? \"\" : \",\", i, mtProfileEffectiveWorkerCountHist[i]);\n");
  emitBodyLock(1, "fprintf(stderr, \"\\n\");\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] partition_facts batch_member_node_count=%%lu same_active_word_forward_edges=%%lu cross_batch_activation_fanout=%%lu\\n\", mtProfileBatchMemberNodeCount, mtProfileSameActiveWordForwardEdges, mtProfileCrossBatchActivationFanout);\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] worker_task_count=\");\n");
  emitBodyLock(1, "for (size_t i = 0; i < mtProfileWorkerTaskCount.size(); i ++) fprintf(stderr, \"%%s%%lu\", i == 0 ? \"\" : \",\", mtProfileWorkerTaskCount[i]);\n");
  emitBodyLock(1, "fprintf(stderr, \"\\n\");\n");
  emitBodyLock(1, "const char *taskProfileEnv = getenv(\"GSIM_MT_PROFILE_TASKS\");\n");
  emitBodyLock(1, "if (taskProfileEnv != nullptr && taskProfileEnv[0] != '\\0' && taskProfileEnv[0] != '0') {\n");
  emitBodyLock(2, "fprintf(stderr, \"[mt-profile] task_cpp_ids=\");\n");
  emitBodyLock(2, "bool firstTask = true;\n");
  emitBodyLock(2, "for (int i = 0; i < %d; i ++) {\n", superId);
  emitBodyLock(3, "if (mtProfileTaskExecCount[i] == 0) continue;\n");
  emitBodyLock(3, "fprintf(stderr, \"%%s%%d:%%lu\", firstTask ? \"\" : \",\", i, mtProfileTaskExecCount[i]);\n");
  emitBodyLock(3, "firstTask = false;\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "fprintf(stderr, \"\\n\");\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(0, "}\n");

  /* activation all nodes for reset */
  fprintf(header, "void activateAll();\n");
  emitFuncDecl(0, "void S%s::activateAll() {\n"
               "  memset(activeFlags, 0xff, sizeof(activeFlags));\n"
               "}\n", name.c_str());

   /* input/output interface */
  for (Node* node : input) {
    fprintf(header, "void set_%s(%s val);\n", node->name.c_str(), widthUType(node->width).c_str());
    genInterfaceInput(node);
  }
  for (Node* node : output) {
    fprintf(header, "%s get_%s();\n", widthUType(node->width).c_str(), node->name.c_str());
    genInterfaceOutput(node);
  }

  /* reset functions */
  fprintf(header, "void resetAll();\n");
  genResetAll();
  for (int i = 0; i < resetFuncNum; i ++) {
    fprintf(header, "void subReset%d();\n", i);
    if (globalConfig.MtHelperMode == "buffered-seq") fprintf(header, "void subReset%d(ActiveBuffer &nextActive);\n", i);
    if (useMtHelpers) fprintf(header, "void subReset%d(ActivationDelta &nextActive);\n", i);
  }

  /* main evaluation loop (step) */
  int subStepIdxMax = useMtHelpers ? genActivateMtHelpers() : (useSeqHelpers ? genActivateSeqHelpers(useBufferedHelpers) : genActivate());
  for (int i = 0; i <= subStepIdxMax; i ++) {
    fprintf(header, "void subStep%d();\n", i);
  }
  if (useHelperTasks) {
    for (int i = 0; i < superId; i ++) {
      if (globalConfig.MtHelperMode == "buffered-seq") fprintf(header, "void mtTask%d(uint%d_t &flag, ActiveBuffer &nextActive);\n", i, ACTIVE_WIDTH);
      if (useMtHelpers) fprintf(header, "void mtTask%d(uint%d_t &flag, ActivationDelta &nextActive);\n", i, ACTIVE_WIDTH);
      if (!useBufferedHelpers || useMtHelpers) fprintf(header, "void mtTask%d(uint%d_t &flag);\n", i, ACTIVE_WIDTH);
    }
    if (useMtHelpers) {
      for (int i = 0; i < superId; i ++) {
        if (mtRepCutHeaderTasks[i].repcutRuntimeApplied) {
          fprintf(header, "void mtRepCutLiteTask%d(uint%d_t &flag, ActivationDelta &nextActive);\n", i, ACTIVE_WIDTH);
        }
      }
    }
    if (useMtHelpers) {
      int shardCount = mtPureBatchShardCount();
      for (int shard = 0; shard < shardCount; shard ++) {
        fprintf(header, "void mtRunPureBatchDirectShard%d(int chunkBegin, int chunkEnd, uint%d_t &activeWord);\n", shard, ACTIVE_WIDTH);
        fprintf(header, "void mtRunPureBatchWorkerShard%d(int worker, int chunkBegin, int chunkEnd, std::vector<std::vector<int>> &mtProfileLocalTaskIds, std::vector<uint64_t> &mtProfileLocalWorkerTaskCount);\n", shard);
      }
      fprintf(header, "void mtRunPureBatchWorkerRange(int worker, int chunkBegin, int chunkEnd);\n");
      fprintf(header, "void mtWorkerPoolLoop(int worker);\n");
      fprintf(header, "void startMtWorkerPool();\n");
      fprintf(header, "void stopMtWorkerPool();\n");
      fprintf(header, "void mtRunPureBatch(int beginCppId, int endCppId, uint%d_t &activeWord);\n", ACTIVE_WIDTH);
      if (useCoarseMt) {
        fprintf(header, "void mtRunCoarseLayerWorkerRange(int worker, int regionIndex, int layerIndex, int chunkBegin, int chunkEnd);\n");
        fprintf(header, "void mtMergeLocalCoarseDelta(int worker, int regionBeginActiveWord, int regionActiveWordSpan);\n");
        fprintf(header, "void mtRunCoarseMTaskWorkerRange(int worker, int regionIndex, int mtaskBegin, int mtaskEnd);\n");
        fprintf(header, "void mtRunCoarseRegion(int regionIndex, uint%d_t *coarseActiveWords);\n", ACTIVE_WIDTH);
      }
    }
  }

  /* step wrapper */
  fprintf(header, "void step();\n");
  genStep(subStepIdxMax);

  /* end of file */
  fprintf(header, "};\n"
                  "#endif\n");
  fclose(header);
  fclose(srcFp);
#ifdef DIFFTEST_PER_SIG
  fclose(sigFile);
#endif

  printf("[cppEmitter] define %ld nodes %d superNodes\n", definedNode.size(), superId);
  std::cout << "[cppEmitter] finish writing " << srcFileIdx << " cpp files to " + globalConfig.OutputDir + "/" << std::endl;
}
