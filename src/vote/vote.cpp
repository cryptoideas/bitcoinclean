
#include "validation.h"
#include "consensus/validation.h"
#include "vote/vote.h"
#include "vote/votedb.h"
#include "base58.h"
#include <regex>

CScoreKeeper keeper;
CVoteDB *voteDB = nullptr;

const std::string ScriptToString(const CScript &script, int n)
{
  if(n == 0)
    n = script.size();
  std::string str;
  str.reserve(n);
  for (int i = 0; i < n; ++i)
    str+= (char) script[i];
  return str;
}

bool IsVoteTx(const CTransaction &tx)
{
  if (tx.vout.size() > 2)
    return false;
  if (!tx.vout[0].scriptPubKey.IsUnspendable())
    return false;
  if (std::string::npos == ScriptToString(tx.vout[0].scriptPubKey, 10).find("CLEAN"))
    return false;
  return true;
}

bool ValidateVoteTx(const CTransaction &tx, CValidationState &state, const CCoinsViewCache &inputs)
{
  CVoteParser parser;
  parser.Init(ScriptToString(tx.vout[0].scriptPubKey));
  LogPrintf("scanvote detected %s\n", parser.str);
  std::set<CKeyID> targetHashes;
  std::vector<CVote> votes;

  while(!parser.done) {
    parser.Next();
    std::pair<std::set<CKeyID>::iterator,bool> ins = targetHashes.insert(parser.result.targetHash);
    if (!ins.second)
      return state.Invalid(false, REJECT_VOTE_INVALID, strprintf("repeated-vote-target (%s)", EncodeDestination(parser.result.targetHash)));
    votes.push_back(parser.result);
  }

  if (!parser.valid)
    return state.Invalid(false, REJECT_VOTE_INVALID, strprintf("invalid-vote-syntax (%i %s)", parser.pos, parser.token));
  if (parser.result.sourceHash == parser.result.targetHash)
    return state.Invalid(false, REJECT_VOTE_SELF, strprintf("self-vote (%s)", EncodeDestination(parser.result.sourceHash)));
  if (!IsVoteAuthentic(tx, parser.result.sourceHash, inputs))
    return state.Invalid(false, REJECT_VOTE_INAUTHENTIC, strprintf("inauthentic-vote (%s)", EncodeDestination(parser.result.targetHash)));
  if (!SufficientMinerrank(parser.result.sourceHash))
    return state.Invalid(false, REJECT_VOTE_INSUFFICIENT, strprintf("insufficient-minerrank (%s)", EncodeDestination(parser.result.sourceHash)));

  for (auto it = votes.begin(); it != votes.end(); it++) {
    if (IsVoteInMempool(*it))
      return state.Invalid(false, REJECT_VOTE_MEMPOOL, strprintf("vote-in-mempool (%s)", it->ToString()));
    if (IsVoteLocked(*it))
      return state.Invalid(false, REJECT_VOTE_LOCKED, strprintf("locked-vote (%s %i until %i)", it->ToString(), keeper.voteLock[*it], keeper.voteLock[*it] + VOTE_WINDOW));
  }

  return true;
}

// blockchain does not store prevout sender
// therefore, protocol stores sending address in null data area
// this makes sure that sending address matches the one in tx for validity
bool IsVoteAuthentic(const CTransaction &tx, const CKeyID &sourceHash, const CCoinsViewCache &inputs)
{
  const Coin& coin = inputs.AccessCoin(tx.vin[0].prevout);
  CTxDestination destination;
  ExtractDestination(coin.out.scriptPubKey, destination);
  CKeyID *hash;
  hash = boost::get<CKeyID>(&destination);
  if (!hash) {
    LogPrintf("Warning: Vote from nonlegacy %s\n", EncodeDestination(destination));
    return false;
  }
  return *hash == sourceHash;
}

bool IsVoteLocked(const CVote &vote)
{
  return keeper.voteLock.find(vote) != keeper.voteLock.end();
}

bool IsVoteInMempool(const CVote &vote)
{
  return voteDB->HasMempoolLock(vote);
}

bool SufficientMinerrank(const CKeyID &hash)
{
  return keeper.Sufficient(hash);
}

int VoteType(std::string str)
{
  size_t pos;
  pos = str.find("UP");
  if (pos != std::string::npos) {
    return VOTE_UP;
  }
  pos = str.find("DOWN");
  if (pos != std::string::npos) {
    return VOTE_DOWN;
  }
  return 0;
}

inline std::string VoteTypeStr(const int &type) {
  switch (type) {
  case VOTE_UP:
    return "UP";
  case VOTE_DOWN:
    return "DOWN";
  }
  return 0;
}

std::string CScore::ToString() const
{
  return strprintf("CScore(weight=%d minerrank=%d)", weight, minerrank);
}

void CVote::Invert()
{
  if (type == VOTE_UP)
    type = VOTE_DOWN;
  else
    type = VOTE_UP;
}

std::string CVote::ToString() const
{
  return strprintf("CLEAN %s %s %s", EncodeDestination(sourceHash), VoteTypeStr(type), EncodeDestination(targetHash));
}

bool operator<(const CVote &v1, const CVote &v2)
{
    if (v1.sourceHash < v2.sourceHash)  return true;
    if ( ! (v1.sourceHash == v2.sourceHash))  return false; // ersatz >
    // source equal

    if (v1.type < v2.type)  return true;
    if (v1.type > v2.type)  return false;
    // type equal

    if (v1.targetHash < v2.targetHash)  return true;
    if ( ! (v1.targetHash == v2.targetHash))  return false; // ersatz >
    // target equal

    // all equal
    return false;
}

void CScoreKeeper::UpdateActive(double last, double current)
{
  int lastActive = active;
  if (last < VOTE_MINERRANK_CUTOFF && current >= VOTE_MINERRANK_CUTOFF)
    ++active;
  else if (current < VOTE_MINERRANK_CUTOFF && last >= VOTE_MINERRANK_CUTOFF)
    --active;
  if (lastActive < VOTE_MIN_ACTIVE && ! (active < VOTE_MIN_ACTIVE))
    LogPrintf("fallback mode disabled ");
  if (active < VOTE_MIN_ACTIVE && ! (lastActive < VOTE_MIN_ACTIVE))
    LogPrintf("fallback mode enabled ");
}

void CScoreKeeper::Add(const CVote &vote)
{
  LogPrintf("add vote old %s new %s active %i ", vote.ToString(), scores[vote.targetHash].ToString(), active);

  double last = scores[vote.targetHash].minerrank;
  double rankchange;
  if (vote.type == VOTE_UP) {
    rankchange = 20;
  } else {
    rankchange = -20;
  }
  if ( ! (active < VOTE_MIN_ACTIVE)) {
    rankchange *= scores[vote.sourceHash].weight;
  } else {
    LogPrintf("fallback weight override ");
  }
  scores[vote.targetHash].minerrank += rankchange;

  UpdateActive(last, scores[vote.targetHash].minerrank);

  LogPrintf("new %s active %i ", scores[vote.targetHash].ToString(), active);

  // run through confirmees, dole out weight
  for (auto it = confirmees[vote.targetHash].begin(); it != confirmees[vote.targetHash].end(); ) {
    LogPrintf("weight %s thanks to %s on %s", EncodeDestination(it->hash).substr(0,8), EncodeDestination(vote.sourceHash).substr(0,8), EncodeDestination(vote.targetHash).substr(0,8));
    if (it->height + VOTE_WINDOW < height) {
      LogPrintf("apply window %s on %i ", EncodeDestination(it->hash), it->height);
      it = confirmees[vote.targetHash].erase(it);
      continue;
    }

    if (vote.type == it->type) {
      LogPrintf("old %d ", scores[it->hash].weight);
      scores[it->hash].weight += scores[it->hash].delta / VOTE_WEIGHT_CYCLE; // linear interpolate
      if (scores[it->hash].cycle >= VOTE_WEIGHT_CYCLE) {
        scores[it->hash].delta =  scores[it->hash].delta * VOTE_WEIGHT_FACTOR;
        scores[it->hash].cycle = 0;
        LogPrintf("new %d ", scores[it->hash].weight);
      } else {
        LogPrintf("hold at %i for cycle %i", scores[it->hash].weight, scores[it->hash].cycle);
      }
      scores[it->hash].cycle += 1;
    }
    LogPrintf("\n");
    ++it;
  }

  confirmee_t confirmee;
  confirmee.height = height;
  confirmee.hash = vote.sourceHash;
  confirmee.type = vote.type;

  confirmees[vote.targetHash].push_back(confirmee);
  LogPrintf("reg %s to receive weight on confirm %s ", EncodeDestination(vote.sourceHash).substr(0,8), EncodeDestination(vote.targetHash).substr(0,8));

  voteLock[vote] = height;
  LogPrintf("lock at %i to %i", height, height + VOTE_WINDOW);

  CVote inverse = vote;
  inverse.Invert();
  auto it = voteLock.find(inverse);
  if (it != voteLock.end()) {
    LogPrintf("unlock inverse at %i to %i ", it->second, it->second + VOTE_WINDOW);
    voteLock.erase(it);
  }
  LogPrintf("\n");
}

void CScoreKeeper::Window()
{
  // maintain window for blocking repeat votes
  for (auto it = voteLock.begin(); it != voteLock.end(); ) {
    if (it->second + VOTE_WINDOW < height)
      it = voteLock.erase(it);
    else
      ++it;
  }
}

bool CScoreKeeper::Sufficient(const CKeyID &hash)
{
  // emergency fallback - if there are ever fewer than three miners with sufficient score,
  // open for all and hope for the best
  if (active < VOTE_MIN_ACTIVE) {
    LogPrintf("fallback approve %s active %i ", EncodeDestination(hash), active);
    return true;
  }
  auto it = scores.find(hash);
  if (it == scores.end())
    return false;
  return it->second.minerrank >= VOTE_MINERRANK_CUTOFF;
}

void CScoreKeeper::Attrit()
{
  LogPrintf("applying block attrition to minerrank\n");
  for (auto it = scores.begin(); it != scores.end();++it) {
    double last = it->second.minerrank;
    it->second.minerrank /= VOTE_ATTRITION_RATE;
    UpdateActive(last, it->second.minerrank);
  }
}

std::string CScoreKeeper::ToString() const
{
  std::string str;
  for(auto it=scores.begin(); it != scores.end(); ++it) {
    str += strprintf("%s %s ", EncodeDestination(it->first).substr(0, 8), it->second.ToString());
  }
  return strprintf("CScoreKeeper(height=%i active=%i, scores= %s)", height, active, str);
}

void CVoteParser::Init(const std::string s)
{
  str = s;
  pos = str.find("CLEAN");
  if (pos == std::string::npos) {
    done = true;
    return;
  }
  next = str.find(" ", pos);
  valid = false;
  done = false;
  if (next == std::string::npos) {
    done = true;
    return;
  }
  pos = next + 1;
  next = str.find(" ", pos);
  if (next == std::string::npos) {
    token = str.substr(pos);
    done = true;
  } else {
    token = str.substr(pos, next-pos);
  }
  CTxDestination destination = DecodeDestination(token);
  if (!IsValidDestination(destination)) {
    done = true;
    return;
  }
  const CKeyID* hash = boost::get<CKeyID>(&destination);
  if (!hash) {
    done = true;
    return;
  }
  result.sourceHash = *hash;
  if (next == std::string::npos)
    pos = str.length();
}

void CVoteParser::Next()
{
  pos = next + 1;
  next = str.find(" ", pos);
  if (next == std::string::npos) {
    token = str.substr(pos);
    done = true;
  } else {
    token = str.substr(pos, next-pos);
  }

  if (token == "UP") {
    type = VOTE_UP;
    valid = false;
    if (next == std::string::npos)
      pos = str.length();
    else
      Next();
    return;
  } else if (token == "DOWN") {
    type = VOTE_DOWN;
    valid = false;
    if (next == std::string::npos)
      pos = str.length();
    else
      Next();
    return;
  }

  result.type = type;

  CTxDestination destination = DecodeDestination(token);
  if (!IsValidDestination(destination)) {
    done = true;
    return;
  }
  const CKeyID* hash = boost::get<CKeyID>(&destination);
  if (!hash) {
    done = true;
    return;
  }
  result.targetHash = *hash;
  valid = true;
  if (next == std::string::npos)
    pos = str.length();
}

void ProcessBlock(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex *pindex)
{
  keeper.height = pindex->nHeight;
  LogPrintf("clearing old locks ");
  keeper.Window();
  LogPrintf("applying per-block minerrank attrition ");
  keeper.Attrit();
  for (size_t i = 0; i < pblock->vtx.size(); i++) {
    if (!IsVoteTx(*pblock->vtx[i]))
      continue;

    CVoteParser parser;
    CTransaction tx = *pblock->vtx[i];
    parser.Init(ScriptToString(tx.vout[0].scriptPubKey));

    std::vector<CVote> votes;
    LogPrintf("scanvote in block %s ", parser.str);
    while(!parser.done) {
      parser.Next();
      votes.push_back(parser.result);
    }
    if (parser.valid) {
      LogPrintf("valid\n");
      for(auto it = votes.begin(); it != votes.end(); ++it) {
        LogPrintf("vote %s in block unlocking mempool ", it->ToString());
        voteDB->MempoolUnlock(*it);
        keeper.Add(*it);
      }
    } else {
      LogPrintf("invalid\n");
    }
  }
}

void CVoteInterface::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex *pindex, const std::vector<CTransactionRef>& vtxConflicted)
{
  // todo - compare height and reload
  LOCK(cs_main);
  LogPrintf("handlevote connect height %i keeper %s\n", chainActive.Tip()->nHeight, keeper.ToString());
  ProcessBlock(pblock, pindex);
  voteDB->Write(chainActive.Tip()->nHeight, keeper);
}

void CVoteInterface::BlockDisconnected(const std::shared_ptr<const CBlock>& pblock)
{
  LogPrintf("handlevote disconnect height %i keeper %s\n", chainActive.Tip()->nHeight, keeper.ToString());
  voteDB->Erase(chainActive.Tip()->nHeight+1);
  voteDB->Read(chainActive.Tip()->nHeight, keeper);
}

void CVoteInterface::TransactionAddedToMempool(const CTransactionRef& ptx)
{
  CTransaction tx = *ptx;
  if (IsVoteTx(tx)) {
    CVoteParser parser;
    parser.Init(ScriptToString(tx.vout[0].scriptPubKey));
    while(!parser.done) {
      parser.Next();
      LogPrintf("vote %s in mempool, locking\n", parser.result.ToString());
      voteDB->MempoolLock(parser.result);
    }
  }
}

void SlowScanVote()
{
  CBlockIndex* pindex = nullptr;
  if (chainActive.Tip()->nHeight >= VOTE_HEIGHT_CUTOFF)
    // optimize for main chain- no votes before this height
    pindex = chainActive.Tip()->GetAncestor(VOTE_HEIGHT_CUTOFF);
  else
    // support regtest and test chain
    pindex = chainActive.Genesis();
  {
    LOCK(cs_main);
    bool abort = false;
    while (pindex && !abort)
    {
      std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
      CBlock& block = *pblock;
      if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
        ProcessBlock(pblock, pindex);
      }
      pindex = chainActive.Next(pindex);
    }
  }
}

void InitVote()
{
  voteDB = new CVoteDB();
  CVoteInterface* ai = new CVoteInterface();
  RegisterValidationInterface(ai);

  // testing- empty directory
  if (chainActive.Tip()==NULL)
      return;

  if (voteDB->Read(chainActive.Tip()->nHeight, keeper)) {
    LogPrintf("Loaded voteDB\n");
  } else {
    LogPrintf("Could not load voteDB, performing slow scan...");
    SlowScanVote();
    voteDB->Write(chainActive.Tip()->nHeight, keeper);
    LogPrintf(" done. saved voteDB\n");
  }
}

bool IsVoteActive()
{
  return keeper.active >= VOTE_MIN_ACTIVE;
}

CScoreKeeper GetScoreKeeper(int height)
{
  CScoreKeeper k;
  if (height == -1)
    return keeper;
  voteDB->Read(height, k);
  return k;
}

bool GetVoteScore(const CKeyID &hash, CScore &score)
{
  auto it=keeper.scores.find(hash);
  if (it == keeper.scores.end())
    return false;
  score = it->second;
  return true;
}

