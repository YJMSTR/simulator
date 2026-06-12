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
  bool hasStateUpdate = false;
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

static MtPureBatchPlan planMtPureBatches(const std::map<int, MtTaskInfo>& tasks, bool allowCuts) {
  MtPureBatchPlan plan;
  for (int idx = 0; idx < superId; idx ++) {
    int id;
    uint64_t mask;
    std::tie(id, mask) = setIdxMask(idx);
    bool activeWhole = true;
    for (int j = 0; j < ACTIVE_WIDTH && idx + j < superId; j ++) {
      if (isAlwaysActive(idx + j)) activeWhole = false;
    }
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
  includeLib(header, "chrono", true);
  includeLib(header, "cstdlib", true);
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
  fprintf(header,
          "struct ActiveBuffer {\n"
          "  uint%d_t words[%d];\n"
          "  void clear() {\n"
          "    memset(words, 0, sizeof(words));\n"
          "  }\n"
          "  void orWord(int idx, uint64_t mask) {\n"
          "    for (int i = 0; i < 8 && idx + i < %d; i ++) {\n"
          "      words[idx + i] |= (uint%d_t)(mask >> (i * %d));\n"
          "    }\n"
          "  }\n"
          "  void activateAll() {\n"
          "    memset(words, 0xff, sizeof(words));\n"
          "  }\n"
          "  void mergeFrom(uint%d_t *activeFlags) const {\n"
          "    for (int i = 0; i < %d; i ++) activeFlags[i] |= words[i];\n"
          "  }\n"
          "};\n\n",
          ACTIVE_WIDTH, activeWords, activeWords, ACTIVE_WIDTH, ACTIVE_WIDTH, ACTIVE_WIDTH, activeWords);
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

void graph::genMtTaskHelper(SuperNode* super, bool buffered) {
  if (buffered) {
    emitFuncDecl(0, "void S%s::mtTask%d(uint%d_t &flag, ActiveBuffer &nextActive) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH);
    genSuperEval(super, "flag", "nextActive", 1);
  } else {
    emitFuncDecl(0, "void S%s::mtTask%d(uint%d_t &flag) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH);
    genSuperEval(super, "flag", "", 1);
  }
  emitBodyLock(0, "}\n");
}

void graph::genMtRepCutLiteTaskHelper(SuperNode* super, const std::vector<MtRepCutClone>& clones) {
  emitFuncDecl(0, "void S%s::mtRepCutLiteTask%d(uint%d_t &flag, ActiveBuffer &nextActive) {\n", name.c_str(), super->cppId, ACTIVE_WIDTH);
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
  emitFuncDecl(0, "void S%s::mtRunPureBatch(int beginCppId, int endCppId, uint%d_t &activeWord) {\n", name.c_str(), ACTIVE_WIDTH);
  emitBodyLock(1, "int taskCount = endCppId - beginCppId;\n");
  emitBodyLock(1, "if (taskCount <= 0) return;\n");
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileBatchBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "const char *threadsEnv = getenv(\"GSIM_THREADS\");\n");
  emitBodyLock(1, "int workerCount = threadsEnv == nullptr ? 1 : atoi(threadsEnv);\n");
  emitBodyLock(1, "if (workerCount < 1) workerCount = 1;\n");
  bool hasForcedSerialBatch = false;
  for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
    if (batch.forcedSerial) hasForcedSerialBatch = true;
  }
  if (hasForcedSerialBatch) {
    emitBodyLock(1, "switch (beginCppId) {\n");
    for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
      if (batch.forcedSerial) emitBodyLock(2, "case %d:\n", batch.beginCppId);
    }
    emitBodyLock(3, "workerCount = 1;\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(2, "default:\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(1, "}\n");
  }
  emitBodyLock(1, "if (workerCount > taskCount) workerCount = taskCount;\n");
  emitBodyLock(1, "if (mtProfileEnabled && workerCount > mtProfileMaxWorkerCount) mtProfileMaxWorkerCount = workerCount;\n");
  emitBodyLock(1, "if (mtProfileEnabled && mtProfileWorkerTaskCount.size() < (size_t)workerCount) mtProfileWorkerTaskCount.resize((size_t)workerCount, 0);\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfilePureBatchCount ++;\n");
  emitBodyLock(1, "std::vector<uint64_t> mtProfileLocalWorkerTaskCount(workerCount, 0);\n");
  emitBodyLock(1, "std::vector<std::vector<uint64_t>> mtProfileLocalTaskExec;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileLocalTaskExec.assign(workerCount, std::vector<uint64_t>(%d, 0));\n", superId);
  emitBodyLock(1, "std::vector<ActiveBuffer> workerBuffers(workerCount);\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) workerBuffers[worker].clear();\n");
  emitBodyLock(1, "std::vector<uint%d_t> workerFlags(workerCount, activeWord);\n", ACTIVE_WIDTH);
  if (!semanticPlan.cutBatches.empty()) {
    emitBodyLock(1, "switch (beginCppId) {\n");
    for (const MtRepCutBatch& batch : semanticPlan.cutBatches) {
      uint64_t forcedSinkMask = mtRepCutForcedSinkMaskForBatch(semanticPlan, batch.beginCppId);
      if (forcedSinkMask != 0) {
        emitBodyLock(2, "case %d:\n", batch.beginCppId);
        emitBodyLock(3, "for (int worker = 0; worker < workerCount; worker ++) workerFlags[worker] |= 0x%lx;\n", forcedSinkMask);
        emitBodyLock(3, "break;\n");
      }
    }
    emitBodyLock(2, "default:\n");
    emitBodyLock(3, "break;\n");
    emitBodyLock(1, "}\n");
  }
  emitBodyLock(1, "auto runWorker = [&](int worker, int chunkBegin, int chunkEnd) {\n");
  emitBodyLock(2, "for (int cppId = chunkBegin; cppId < chunkEnd; cppId ++) {\n");
  emitBodyLock(3, "switch (cppId) {\n");
  for (int cppId = 0; cppId < superId; cppId ++) {
    if (mtTasks[cppId].taskKind != "pure_compute") continue;
    emitBodyLock(4, "case %d:\n", cppId);
    emitBodyLock(5, "if (workerFlags[worker] & 0x%lx) {\n", (uint64_t)1 << (cppId % ACTIVE_WIDTH));
    emitBodyLock(6, "std::chrono::steady_clock::time_point mtProfileTaskBegin;\n");
    emitBodyLock(6, "if (mtProfileEnabled) mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
    if (mtTasks[cppId].repcutRuntimeApplied) {
      emitBodyLock(6, "mtRepCutLiteTask%d(workerFlags[worker], workerBuffers[worker]);\n", cppId);
    } else {
      emitBodyLock(6, "mtTask%d(workerFlags[worker], workerBuffers[worker]);\n", cppId);
    }
    emitBodyLock(6, "if (mtProfileEnabled) {\n");
    emitBodyLock(7, "mtProfileLocalTaskExec[worker][%d] ++;\n", cppId);
    emitBodyLock(7, "mtProfileLocalWorkerTaskCount[worker] ++;\n");
    emitBodyLock(6, "}\n");
    emitBodyLock(5, "}\n");
    emitBodyLock(5, "break;\n");
  }
  emitBodyLock(4, "default:\n");
  emitBodyLock(5, "break;\n");
  emitBodyLock(3, "}\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "};\n");
  emitBodyLock(1, "if (workerCount == 1) {\n");
  emitBodyLock(2, "runWorker(0, beginCppId, endCppId);\n");
  emitBodyLock(1, "} else {\n");
  emitBodyLock(2, "std::vector<std::thread> workers;\n");
  emitBodyLock(2, "workers.reserve(workerCount);\n");
  emitBodyLock(2, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(3, "int chunkBegin = beginCppId + (taskCount * worker) / workerCount;\n");
  emitBodyLock(3, "int chunkEnd = beginCppId + (taskCount * (worker + 1)) / workerCount;\n");
  emitBodyLock(3, "workers.emplace_back([&, worker, chunkBegin, chunkEnd]() { runWorker(worker, chunkBegin, chunkEnd); });\n");
  emitBodyLock(2, "}\n");
  emitBodyLock(2, "for (std::thread &worker : workers) worker.join();\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "std::chrono::steady_clock::time_point mtProfileMergeBegin;\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeBegin = std::chrono::steady_clock::now();\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) activeWord |= workerFlags[worker];\n");
  emitBodyLock(1, "for (int worker = 0; worker < workerCount; worker ++) workerBuffers[worker].mergeFrom(activeFlags);\n");
  emitBodyLock(1, "if (mtProfileEnabled) {\n");
  emitBodyLock(2, "for (int worker = 0; worker < workerCount; worker ++) {\n");
  emitBodyLock(3, "mtProfileWorkerTaskCount[(size_t)worker] += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(3, "mtProfilePureTasks += mtProfileLocalWorkerTaskCount[worker];\n");
  emitBodyLock(3, "for (int cppId = 0; cppId < %d; cppId ++) mtProfileTaskExecCount[cppId] += mtProfileLocalTaskExec[worker][cppId];\n", superId);
  emitBodyLock(2, "}\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileMergeWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileMergeBegin).count();\n");
  emitBodyLock(1, "if (mtProfileEnabled) mtProfileBatchWallNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileBatchBegin).count();\n");
  emitBodyLock(0, "}\n");
}

int graph::genActivateSeqHelpers(bool buffered) {
    std::map<int, MtTaskInfo> mtTasks = buildMtTaskInfoMapWithRepCut();
    for (int idx = 0; idx < superId; idx ++) {
      genMtTaskHelper(cppId2Super[idx], buffered);
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
    MtPureBatchPlan batchPlan = semanticPlan.batchPlan;
    std::map<int, int> batchEndByStart;
    for (auto batch : batchPlan.batches) {
      batchEndByStart[batch.first] = batch.second;
    }
    for (int idx = 0; idx < superId; idx ++) {
      genMtTaskHelper(cppId2Super[idx], true);
    }
    for (int idx = 0; idx < superId; idx ++) {
      if (mtTasks[idx].repcutRuntimeApplied) genMtRepCutLiteTaskHelper(cppId2Super[idx], mtRepCutClonesForSink(semanticPlan, idx));
    }
    genMtTaskRunner(semanticPlan);

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
      emitBodyLock(indent, "ActiveBuffer mtBuffer;\n");
      emitBodyLock(indent, "mtBuffer.clear();\n");
      emitBodyLock(indent, "std::chrono::steady_clock::time_point mtProfileTaskBegin;\n");
      emitBodyLock(indent, "if (mtProfileEnabled) mtProfileTaskBegin = std::chrono::steady_clock::now();\n");
      emitBodyLock(indent, "mtTask%d(%s, mtBuffer);\n", idx, flagName.c_str());
      emitBodyLock(indent, "recordMtProfileTask(%d, %s, mtProfileEnabled ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mtProfileTaskBegin).count() : 0);\n",
                   idx, mtTasks[idx].taskKind == "pure_compute" ? "true" : "false");
      emitBodyLock(indent, "mtBuffer.mergeFrom(activeFlags);\n");
      indent = genNodeStepEnd(super, indent);
    }
    emitBodyLock(--indent, "}\n");
    if (prevActiveWhole) emitBodyLock(--indent, "}\n");

    return nextSubStepIdx - 1;
}

void graph::genResetDef(SuperNode* super, bool isUIntReset, bool buffered, int resetId, int indent) {
  if (buffered) emitBodyLock(indent ++, "void S%s::subReset%d(ActiveBuffer &nextActive){ // %s reset\n", name.c_str(), resetId, isUIntReset ? "uint" : "async");
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
  std::map<int, MtTaskInfo> mtRepCutHeaderTasks;
  if (useMtHelpers) {
    mtRepCutHeaderTasks = buildMtTaskInfoMapWithRepCutSelection();
    markMtRepCutLiteRuntimeApplied(mtRepCutHeaderTasks);
  }
  if (useBufferedHelpers) emitActiveBufferDef(header, activeFlagNum);

  fprintf(header, "class S%s {\npublic:\n", name.c_str());
  fprintf(header, "uint64_t cycles;\n");
  fprintf(header, "uint64_t LOG_START, LOG_END;\n");
  fprintf(header, "uint%d_t activeFlags[%d];\n", ACTIVE_WIDTH, activeFlagNum); // or super.size() if id == idx
  fprintf(header, "bool mtProfileEnabled;\n");
  fprintf(header, "const char *mtProfileHelperMode;\n");
  fprintf(header, "int mtProfileConfiguredWorkerCount;\n");
  fprintf(header, "int mtProfileMaxWorkerCount;\n");
  fprintf(header, "uint64_t mtProfileActiveWordCount;\n");
  fprintf(header, "uint64_t mtProfileSerialTasks;\n");
  fprintf(header, "uint64_t mtProfilePureTasks;\n");
  fprintf(header, "uint64_t mtProfilePureBatchCount;\n");
  fprintf(header, "uint64_t mtProfileBatchWallNs;\n");
  fprintf(header, "uint64_t mtProfileSerialWallNs;\n");
  fprintf(header, "uint64_t mtProfileMergeWallNs;\n");
  fprintf(header, "uint64_t mtProfileTotalStepNs;\n");
  fprintf(header, "uint64_t mtProfileTaskExecCount[%d];\n", superId);
  fprintf(header, "std::vector<uint64_t> mtProfileWorkerTaskCount;\n");
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
  emitFuncDecl(0, "S%s::S%s() {\n"
               "  cycles = 0;\n"
               "  LOG_START = 1;\n"
               "  LOG_END = 0;\n"
               "  initMtProfile();\n"
               "  init();\n"
               "}\n", name.c_str(), name.c_str());

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
  emitBodyLock(1, "mtProfileConfiguredWorkerCount = threadsEnv == nullptr ? 1 : atoi(threadsEnv);\n");
  emitBodyLock(1, "if (mtProfileConfiguredWorkerCount < 1) mtProfileConfiguredWorkerCount = 1;\n");
  emitBodyLock(1, "mtProfileMaxWorkerCount = 1;\n");
  emitBodyLock(1, "mtProfileActiveWordCount = 0;\n");
  emitBodyLock(1, "mtProfileSerialTasks = 0;\n");
  emitBodyLock(1, "mtProfilePureTasks = 0;\n");
  emitBodyLock(1, "mtProfilePureBatchCount = 0;\n");
  emitBodyLock(1, "mtProfileBatchWallNs = 0;\n");
  emitBodyLock(1, "mtProfileSerialWallNs = 0;\n");
  emitBodyLock(1, "mtProfileMergeWallNs = 0;\n");
  emitBodyLock(1, "mtProfileTotalStepNs = 0;\n");
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) mtProfileTaskExecCount[i] = 0;\n", superId);
  emitBodyLock(1, "mtProfileWorkerTaskCount.assign((size_t)mtProfileConfiguredWorkerCount, 0);\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "S%s::~S%s() {\n", name.c_str(), name.c_str());
  emitBodyLock(1, "dumpMtProfile();\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::recordMtProfileTask(int cppId, bool pureTask, uint64_t elapsedNs) {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  emitBodyLock(1, "if (cppId >= 0 && cppId < %d) mtProfileTaskExecCount[cppId] ++;\n", superId);
  emitBodyLock(1, "if (pureTask) mtProfilePureTasks ++;\n");
  emitBodyLock(1, "else mtProfileSerialTasks ++;\n");
  emitBodyLock(1, "if (!pureTask) mtProfileSerialWallNs += elapsedNs;\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::recordMtProfileWorkerTask(int worker) {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  emitBodyLock(1, "if (worker >= 0 && (size_t)worker < mtProfileWorkerTaskCount.size()) mtProfileWorkerTaskCount[(size_t)worker] ++;\n");
  emitBodyLock(0, "}\n");

  emitFuncDecl(0, "void S%s::dumpMtProfile() {\n", name.c_str());
  emitBodyLock(1, "if (!mtProfileEnabled) return;\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] helper_mode=%%s worker_count=%%d max_worker_count=%%d cycles=%%lu active_word_count=%%lu serial_tasks=%%lu pure_tasks=%%lu pure_batch_count=%%lu batch_wall_ns=%%lu serial_wall_ns=%%lu merge_wall_ns=%%lu total_step_ns=%%lu\\n\", mtProfileHelperMode, mtProfileConfiguredWorkerCount, mtProfileMaxWorkerCount, cycles, mtProfileActiveWordCount, mtProfileSerialTasks, mtProfilePureTasks, mtProfilePureBatchCount, mtProfileBatchWallNs, mtProfileSerialWallNs, mtProfileMergeWallNs, mtProfileTotalStepNs);\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] worker_task_count=\");\n");
  emitBodyLock(1, "for (size_t i = 0; i < mtProfileWorkerTaskCount.size(); i ++) fprintf(stderr, \"%%s%%lu\", i == 0 ? \"\" : \",\", mtProfileWorkerTaskCount[i]);\n");
  emitBodyLock(1, "fprintf(stderr, \"\\n\");\n");
  emitBodyLock(1, "fprintf(stderr, \"[mt-profile] task_cpp_ids=\");\n");
  emitBodyLock(1, "bool firstTask = true;\n");
  emitBodyLock(1, "for (int i = 0; i < %d; i ++) {\n", superId);
  emitBodyLock(2, "if (mtProfileTaskExecCount[i] == 0) continue;\n");
  emitBodyLock(2, "fprintf(stderr, \"%%s%%d:%%lu\", firstTask ? \"\" : \",\", i, mtProfileTaskExecCount[i]);\n");
  emitBodyLock(2, "firstTask = false;\n");
  emitBodyLock(1, "}\n");
  emitBodyLock(1, "fprintf(stderr, \"\\n\");\n");
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
    if (useBufferedHelpers) fprintf(header, "void subReset%d(ActiveBuffer &nextActive);\n", i);
  }

  /* main evaluation loop (step) */
  int subStepIdxMax = useMtHelpers ? genActivateMtHelpers() : (useSeqHelpers ? genActivateSeqHelpers(useBufferedHelpers) : genActivate());
  for (int i = 0; i <= subStepIdxMax; i ++) {
    fprintf(header, "void subStep%d();\n", i);
  }
  if (useHelperTasks) {
    for (int i = 0; i < superId; i ++) {
      if (useBufferedHelpers) fprintf(header, "void mtTask%d(uint%d_t &flag, ActiveBuffer &nextActive);\n", i, ACTIVE_WIDTH);
      else fprintf(header, "void mtTask%d(uint%d_t &flag);\n", i, ACTIVE_WIDTH);
    }
    if (useMtHelpers) {
      for (int i = 0; i < superId; i ++) {
        if (mtRepCutHeaderTasks[i].repcutRuntimeApplied) {
          fprintf(header, "void mtRepCutLiteTask%d(uint%d_t &flag, ActiveBuffer &nextActive);\n", i, ACTIVE_WIDTH);
        }
      }
    }
    if (useMtHelpers) fprintf(header, "void mtRunPureBatch(int beginCppId, int endCppId, uint%d_t &activeWord);\n", ACTIVE_WIDTH);
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
