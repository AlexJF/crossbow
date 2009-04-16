/*
 * range_source.h
 */

#ifndef RANGE_SOURCE_H_
#define RANGE_SOURCE_H_

#include <stdint.h>
#include <vector>
#include "seqan/sequence.h"
#include "ebwt.h"
#include "range.h"
#include "pool.h"

enum AdvanceUntil {
	ADV_FOUND_RANGE = 1,
	ADV_COST_CHANGES,
	ADV_STEP
};

/**
 * Encapsulates an edit between the read sequence and the reference
 * sequence.
 */
struct Edit {
	uint16_t type      :  2; // 1 -> subst, 2 -> ins, 3 -> del, 0 -> empty
	uint16_t pos       : 10; // position w/r/t search root
	uint16_t chr       :  2; // character involved (for subst and ins)
	uint16_t reserved  :  2; // reserved
};

/**
 * Expandable list of Edits.  One can:
 * - Add an Edit, which might trigger an expansion
 * -
 */
struct EditList {

	EditList() : sz_(0), moreEdits_(NULL), yetMoreEdits_(NULL) { }

	/**
	 * Add an edit to the edit list.
	 */
	void add(const Edit& e, AllocOnlyPool<Edit>& pool_, size_t qlen) {
		assert_lt(sz_, qlen+3);
		if(sz_ < numEdits) {
			assert(moreEdits_ == NULL);
			assert(yetMoreEdits_ == NULL);
			edits_[sz_++] = e;
		} else if(sz_ == numEdits) {
			assert(moreEdits_ == NULL);
			assert(yetMoreEdits_ == NULL);
			moreEdits_ = pool_.alloc(numMoreEdits);
			assert(moreEdits_ != NULL);
			moreEdits_[0] = e;
			sz_++;
		} else if(sz_ < (numEdits + numMoreEdits)) {
			assert(moreEdits_ != NULL);
			assert(yetMoreEdits_ == NULL);
			moreEdits_[sz_ - numEdits] = e;
			sz_++;
		} else if(sz_ == (numEdits + numMoreEdits)) {
			assert(moreEdits_ != NULL);
			assert(yetMoreEdits_ == NULL);
			yetMoreEdits_ = pool_.alloc(qlen+3 - numMoreEdits - numEdits);
			assert(yetMoreEdits_ != NULL);
			yetMoreEdits_[0] = e;
			sz_++;
		} else {
			assert(moreEdits_ != NULL);
			assert(yetMoreEdits_ != NULL);
			yetMoreEdits_[sz_ - numEdits - numMoreEdits] = e;
			sz_++;
		}
	}

	/**
	 * Return a const reference to the ith Edit in the list.
	 */
	const Edit& get(size_t i) const {
		assert_lt(i, sz_);
		if(i < numEdits) {
			return edits_[i];
		} else if(i < (numEdits + numMoreEdits)) {
			assert(moreEdits_ != NULL);
			return moreEdits_[i-numEdits];
		} else {
			assert(moreEdits_ != NULL);
			assert(yetMoreEdits_ != NULL);
			return yetMoreEdits_[i-numEdits-numMoreEdits];
		}
	}

	/**
	 * Set a particular element of the EditList.
	 */
	void set(size_t i, const Edit& e) {
		assert_lt(i, sz_);
		if(i < numEdits) {
			edits_[i] = e;
		} else if(i < (numEdits + numMoreEdits)) {
			assert(moreEdits_ != NULL);
			moreEdits_[i-numEdits] = e;
		} else {
			assert(moreEdits_ != NULL);
			assert(yetMoreEdits_ != NULL);
			yetMoreEdits_[i-numEdits-numMoreEdits] = e;
		}
	}

	/**
	 * Remove all Edits from the list.
	 */
	void clear() {
		sz_ = 0;
		moreEdits_ = NULL;
		yetMoreEdits_ = NULL;
	}

	/**
	 * Return number of Edits in the List.
	 */
	size_t size() const {
		return sz_;
	}

	const static size_t numEdits = 6;
	const static size_t numMoreEdits = 16;
	size_t sz_;          // number of Edits stored in the EditList
	Edit edits_[numEdits]; // first 4 edits; typically, no more are needed
	Edit *moreEdits_;    // if used, size is dictated by numMoreEdits
	Edit *yetMoreEdits_; // if used, size is dictated by length of read
};

/**
 * 3 types of edits; mismatch (substitution), insertion in the
 * reference, deletion in the reference.
 */
enum {
	EDIT_TYPE_MM = 1,
	EDIT_TYPE_INS,
	EDIT_TYPE_DEL
};

/**
 * Holds per-position information about what outgoing paths have been
 * eliminated and what the quality value is at the position.
 */
union ElimsAndQual {

	/**
	 * Set all the non-qual bits of the flags field to 1, indicating
	 * that all outgoing paths are eliminated.
	 */
	inline void eliminate() {
		join.elims = 511;
	}

	struct {
		uint16_t mmA   : 1; // A in ref aligns to non-A char in read
		uint16_t mmC   : 1; // C in ref aligns to non-C char in read
		uint16_t mmG   : 1; // G in ref aligns to non-G char in read
		uint16_t mmT   : 1; // T in ref aligns to non-T char in read
		uint16_t insA  : 1; // A insertion in reference w/r/t read
		uint16_t insC  : 1; // C insertion in reference w/r/t read
		uint16_t insG  : 1; // G insertion in reference w/r/t read
		uint16_t insT  : 1; // T insertion in reference w/r/t read
		uint16_t del   : 1; // deletion of read character
		uint16_t qual  : 7; // quality of position
	} flags;
	struct {
		uint16_t elims : 9; // all of the edit-elim flags bundled together
		uint16_t qual  : 7; // quality of position
	} join;
	struct {
		uint16_t mmElims  : 4; // substitution flags bundled together
		uint16_t insElims : 4; // inserts-in-reference flags bundled together
		uint16_t delElims : 1; // deletion of read character
		uint16_t qual     : 7; // quality of position
	} join2;
};

/**
 * All per-position state, including the ranges calculated for each
 * character, the quality value at the position, and a set of flags
 * recording whether we've tried each way of proceeding from this
 * position.
 */
struct RangeState {

	/**
	 * Using randomness when picking from among multiple choices, pick
	 * an edit to make.  Only knows how to pick mismatches for now.
	 */
	Edit pickEdit(int pos, RandomSource& rand, uint32_t& top,
	              uint32_t& bot, bool& last)
	{
		Edit ret;
		ret.type = EDIT_TYPE_MM;
		ret.pos = pos;
		assert(!eliminated_);
		assert(!eq.flags.mmA || !eq.flags.mmC || !eq.flags.mmG || !eq.flags.mmT);
		int num = !eq.flags.mmA + !eq.flags.mmC + !eq.flags.mmG + !eq.flags.mmT;
		assert_leq(num, 4);
		assert_gt(num, 0);
		if(num > 1) {
			last = false; // not the last at this pos
			// Sum up range sizes and do a random weighted pick
			uint32_t tot = 0;
			if(!eq.flags.mmA) {
				assert_gt(bots[0], tops[0]);
				tot += (bots[0] - tops[0]);
			}
			if(!eq.flags.mmC) {
				assert_gt(bots[1], tops[1]);
				tot += (bots[1] - tops[1]);
			}
			if(!eq.flags.mmG) {
				assert_gt(bots[2], tops[2]);
				tot += (bots[2] - tops[2]);
			}
			if(!eq.flags.mmT) {
				assert_gt(bots[3], tops[3]);
				tot += (bots[3] - tops[3]);
			}
			// Throw a dart randomly that hits one of the possible
			// substitutions, with likelihoods weighted by range size
			uint32_t dart = rand.nextU32() % tot;
			if(!eq.flags.mmA) {
				if(dart < (bots[0] - tops[0])) {
					// Eliminate A mismatch
					top = tops[0];
					bot = bots[0];
					eq.flags.mmA = 1;
					assert_lt(eq.join2.mmElims, 15);
					ret.chr = 0;
					return ret;
				}
				dart -= (bots[0] - tops[0]);
			}
			if(!eq.flags.mmC) {
				if(dart < (bots[1] - tops[1])) {
					// Eliminate C mismatch
					top = tops[1];
					bot = bots[1];
					eq.flags.mmC = 1;
					assert_lt(eq.join2.mmElims, 15);
					ret.chr = 1;
					return ret;
				}
				dart -= (bots[1] - tops[1]);
			}
			if(!eq.flags.mmG) {
				if(dart < (bots[2] - tops[2])) {
					// Eliminate G mismatch
					top = tops[2];
					bot = bots[2];
					eq.flags.mmG = 1;
					assert_lt(eq.join2.mmElims, 15);
					ret.chr = 2;
					return ret;
				}
				dart -= (bots[2] - tops[2]);
			}
			if(!eq.flags.mmT) {
				assert_lt(dart, (bots[3] - tops[3]));
				// Eliminate T mismatch
				top = tops[3];
				bot = bots[3];
				eq.flags.mmT = 1;
				assert_lt(eq.join2.mmElims, 15);
				ret.chr = 3;
			}
			return ret;
		} else {
			last = true; // last at this pos
			// There's only one; pick it!
			if(!eq.flags.mmA) {
				//eq.flags.mmA = 1;
				ret.chr = 0;
			} else if(!eq.flags.mmC) {
				//eq.flags.mmC = 1;
				ret.chr = 1;
			} else if(!eq.flags.mmG) {
				//eq.flags.mmG = 1;
				ret.chr = 2;
			} else {
				assert(!eq.flags.mmT);
				//eq.flags.mmT = 1;
				ret.chr = 3;
			}
			top = tops[ret.chr];
			bot = bots[ret.chr];
			//assert_eq(15, eq.join2.mmElims);
			// Mark entire position as eliminated
			eliminated_ = true;
			return ret;
		}
	}

	/**
	 * Return true (without assertion) iff this RangeState is
	 * internally consistent.
	 */
	bool repOk() {
		// Something has to be eliminated (except when we just matched an N)
		//assert(eliminated_ || eq.flags.mmA || eq.flags.mmC || eq.flags.mmG || eq.flags.mmT);
		//assert(eliminated_ || eq.flags.insA || eq.flags.insC || eq.flags.insG || eq.flags.insT);
		if(eliminated_) return true;
		// Uneliminated chars must have non-empty ranges
		if(!eq.flags.mmA || !eq.flags.insA) assert_gt(bots[0], tops[0]);
		if(!eq.flags.mmC || !eq.flags.insC) assert_gt(bots[1], tops[1]);
		if(!eq.flags.mmG || !eq.flags.insG) assert_gt(bots[2], tops[2]);
		if(!eq.flags.mmT || !eq.flags.insT) assert_gt(bots[3], tops[3]);
		return true;
	}

	// Outgoing ranges; if the position being described is not a
	// legitimate jumping-off point for a branch, tops[] and bots[]
	// will be filled with 0s and all possibilities in eq will be
	// eliminated
	uint32_t tops[4]; // A, C, G, T top offsets
	uint32_t bots[4]; // A, C, G, T bot offsets
	ElimsAndQual eq;  // Which outgoing paths have been tried already
	bool eliminated_;  // Whether all outgoing paths have been eliminated
};

/**
 * Class for managing a pool of memory from which RangeState arrays are
 * allocated.
 */
class RangeStatePool {
	typedef std::pair<uint32_t, uint32_t> U32Pair;
public:
	/**
	 * Initialize a new pool with an initial size of about 'bytes'
	 * bytes.  Exit with an error message if we can't allocate it.
	 */
	RangeStatePool(uint32_t bytes) :
		curPool_(0), cur_(0), lastAlloc_(NULL), lastAllocSz_(0)
	{
		lim_ = bytes / sizeof(RangeState);
		RangeState *pool;
		try {
			pool = new RangeState[lim_];
			if(pool == NULL) throw std::bad_alloc();
		} catch(std::bad_alloc& e) {
			cerr << "Error: Could not allocate RangeStatePool #1 of " << bytes << " bytes";
			exit(1);
		}
		ASSERT_ONLY(memset(pool, 0, lim_ * sizeof(RangeState)));
		pools_.push_back(pool);
	}

	/**
	 * Delete all the pools.
	 */
	~RangeStatePool() {
		for(size_t i = 0; i < pools_.size(); i++) {
			delete[] pools_[i];
		}
	}

	/**
	 * Reset the pool, freeing all arrays that had been given out.
	 */
	void reset() {
#ifndef NDEBUG
		for(size_t i = 0; i < pools_.size(); i++) {
			memset(pools_[i], 0, lim_ * sizeof(RangeState));
		}
#endif
		cur_ = 0;
		curPool_ = 0;
		lastAlloc_ = NULL;
		lastAllocSz_ = 0;
	}


	/**
	 * Rewind to a an old position, essentially freeing everything past
	 * it.
	 */
	void rewind(U32Pair pos) {
		curPool_ = pos.first;
		cur_ = pos.second;
		ASSERT_ONLY(memset(&pools_[curPool_][cur_], 0, (lim_-cur_) * sizeof(RangeState)));
	}

	/**
	 * Return our current position.
	 */
	U32Pair getPos() {
		return make_pair(curPool_, cur_);
	}

	/**
	 * Return the last RangeState allocated from the pool.
	 */
	RangeState* lastAlloc() {
		return lastAlloc_;
	}

	/**
	 * Allocate another array of RangeStates from the pool.
	 */
	RangeState* alloc(uint32_t elts) {
		assert_gt(elts, 0);
		if(cur_ + elts >= lim_) {
			if(curPool_ >= pools_.size()-1) {
				RangeState *pool;
				try {
					pool = new RangeState[lim_];
					if(pool == NULL) throw std::bad_alloc();
				} catch(std::bad_alloc& e) {
					cerr << "Error: Could not allocate RangeStatePool #" << (curPool_+2) << " of " << (lim_ * sizeof(RangeState)) << " bytes";
					exit(1);
				}
				ASSERT_ONLY(memset(pool, 0, lim_ * sizeof(RangeState)));
				pools_.push_back(pool);
			}
			curPool_++;
			cur_ = 0;
			ASSERT_ONLY(memset(pools_[curPool_], 0, lim_ * sizeof(RangeState)));
		}
		lastAlloc_ = &pools_[curPool_][cur_];
		lastAllocSz_ = elts;
		cur_ += elts;
#ifndef NDEBUG
		for(uint32_t i = 0; i < elts; i++) {
			for(int j = 0; j < 4; j++) {
				assert_eq(0, lastAlloc_[i].tops[j]);
				assert_eq(0, lastAlloc_[i].bots[j]);
			}
		}
#endif
		return lastAlloc_;
	}

protected:
	std::vector<RangeState*> pools_; /// the memory pools
	uint32_t    curPool_; /// pool we're current allocating from
	uint32_t    lim_;  /// # elements held in pool_
	uint32_t    cur_;  /// index of next free element of pool_
	RangeState *lastAlloc_; /// last RangeState array allocated
	uint32_t    lastAllocSz_; /// size of last array allocated
};

/**
 * Encapsulates a "branch" of the search space; i.e. all of the
 * information deduced by walking along a path with only matches, along
 * with information about the decisions that lead to the root of that
 * path.
 */
class Branch {
	typedef std::pair<uint32_t, uint32_t> U32Pair;
public:
	Branch() : lastBestCost_(0xffff), curtailed_(false),
	           exhausted_(false), prepped_(false) { }

	/**
	 * Initialize a new branch object with an empty path.
	 */
	void init(RangeStatePool& pool, AllocOnlyPool<Edit>& epool,
	          uint32_t qlen,
	          uint16_t depth0, uint16_t depth1, uint16_t depth2,
	          uint16_t depth3, uint16_t rdepth, uint16_t len,
	          uint16_t cost, uint16_t ham,
	          uint32_t itop, uint32_t ibot,
	          const EbwtParams& ep, const uint8_t* ebwt,
	          const EditList* edits = NULL)
	{
		// No guarantee that there's room in the edits array for all
		// edits; eventually need to do dynamic allocation for them.
		lastBestCost_ = 0xffff;
		depth0_ = depth0;
		depth1_ = depth1;
		depth2_ = depth2;
		depth3_ = depth3;
		rdepth_ = rdepth;
		len_ = len;
		cost_ = cost;
		ham_ = ham;
		top_ = itop;
		bot_ = ibot;
		if(ibot > itop+1) {
			// Care about both top and bot
			SideLocus::initFromTopBot(itop, ibot, ep, ebwt, ltop_, lbot_);
		} else if(ibot > itop) {
			// Only care about top
			ltop_.initFromRow(itop, ep, ebwt);
			lbot_.invalidate();
		}
		if(qlen - rdepth_ > 0) {
			ranges_ = pool.alloc(qlen - rdepth_); // allocated from the RangeStatePool
			assert(ranges_ != NULL);
		} else {
			ranges_ = NULL;
		}
#ifndef NDEBUG
		for(size_t i = 0; i < (qlen - rdepth_); i++) {
			for(int j = 0; j < 4; j++) {
				assert_eq(0, ranges_[i].tops[j]); assert_eq(0, ranges_[i].bots[j]);
			}
		}
#endif
		curtailed_ = false;
		exhausted_ = false;
		prepped_ = true;
		edits_.clear();
		if(edits != NULL) {
			const size_t numEdits = edits->size();
			for(size_t i = 0; i < numEdits; i++) {
				edits_.add(edits->get(i), epool, qlen);
			}
		}
		// If we're starting with a non-zero length, that means we're
		// jumping over a bunch of unrevisitable positions.
		for(size_t i = 0; i < len_; i++) {
			ranges_[i].eliminated_ = true;
			assert(eliminated(i));
		}
		assert(repOk(qlen));
	}

	/**
	 * Depth of the deepest tip of the branch.
	 */
	uint16_t tipDepth() const {
		return rdepth_ + len_;
	}

	/**
	 * Return true iff all outgoing edges from position i have been
	 * eliminated.
	 */
	inline bool eliminated(int i) const {
		if(i <= len_) {
			assert(ranges_ != NULL);
			return ranges_[i].eliminated_;
		}
		return true;
	}

	/**
	 * Split off a new branch by selecting a good outgoing path and
	 * creating a new Branch object for it and inserting that branch
	 * into the priority queue.  Mark that outgoing path from the
	 * parent branch as eliminated.  If the second-best outgoing path
	 * cost more, add the different to the cost of this branch (since
	 * that's the best we can do starting from here from now on).
	 */
	Branch* splitBranch(RangeStatePool& rpool,
	                    AllocOnlyPool<Edit>& epool,
	                    AllocOnlyPool<Branch>& bpool,
	                    Branch *newBranch,
	                    RandomSource& rand, uint32_t qlen, int seedLen,
	                    bool qualOrder, const EbwtParams& ep,
	                    const uint8_t* ebwt)
	{
		assert(!exhausted_);
		assert(ranges_ != NULL);
		assert(curtailed_);
		if(lastBestCost_ == 0) {
			rpool.rewind(lastRpool_);
			epool.rewind(lastEpool_);
			bpool.rewind(lastBpool_);
		}
		int tiedPositions[3];
		int numTiedPositions = 0;
		// Lowest marginal cost incurred by any of the positions with
		// non-eliminated outgoing edges
		uint16_t bestCost = 0xffff;
		// Next-lowest
		uint16_t nextCost = 0xffff;
		int numNotEliminated = 0;
		int i = (int)depth0_;
		i = max(0, i - rdepth_);
		// Iterate over revisitable positions in the path
		for(; i <= len_; i++) {
			// If there are still valid options for leaving out of this
			// position
			if(!eliminated(i)) {
				numNotEliminated++;
				uint16_t stratum = (rdepth_ + i < seedLen) ? (1 << 14) : 0;
				uint16_t cost = (qualOrder ? ranges_[i].eq.join.qual : 0) | stratum;
				if(cost < bestCost) {
					// Demote the old best to the next-best
					nextCost = bestCost;
					// Update the new best
					bestCost = cost;
					numTiedPositions = 1;
					tiedPositions[0] = i;
				} else if(cost == bestCost) {
					// As good as the best so far
					assert_gt(numTiedPositions, 0);
					if(numTiedPositions < 3) {
						tiedPositions[numTiedPositions++] = i;
					} else {
						tiedPositions[0] = tiedPositions[1];
						tiedPositions[1] = tiedPositions[2];
						tiedPositions[2] = i;
					}
				} else if(cost < nextCost) {
					// 'cost' isn't beter than the best, but it is
					// better than the next-best
					nextCost = cost;
				}
			}
		}
		assert_gt(numNotEliminated, 0);
		assert_gt(numTiedPositions, 0);
		if(nextCost != 0xffff) assert_gt(nextCost, bestCost);
		int r = 0;
		// Introduce a little randomness in which path we pick
		if(false && numTiedPositions > 1) {
			// Disabled for now
			r = rand.nextU32() % numTiedPositions;
			assert_geq(r, 0);
			assert_lt(r, 3);
		}
		int pos = tiedPositions[r];
		bool last = false;
		// Pick a not-yet-tried edit (using randomness)
		uint32_t top = 0, bot = 0;
		Edit e = ranges_[pos].pickEdit(pos + rdepth_, rand, top, bot, last);
		assert_gt(bot, top);
		// Create and initialize a new Branch
		uint16_t newRdepth = rdepth_ + pos + 1;
		assert_lt((bestCost >> 14), 4);
		uint16_t depth = pos + rdepth_;
		assert_geq(depth, depth0_);
		uint16_t newDepth0 = depth0_;
		uint16_t newDepth1 = depth1_;
		uint16_t newDepth2 = depth2_;
		uint16_t newDepth3 = depth3_;
		if(depth < depth1_) newDepth0 = depth1_;
		if(depth < depth2_) newDepth1 = depth2_;
		if(depth < depth3_) newDepth2 = depth3_;
		if(bestCost == 0) {
			lastBestCost_ = bestCost;
			lastRpool_ = rpool.getPos();
			lastEpool_ = epool.getPos();
			lastBpool_ = bpool.getPos();
		}
		newBranch->init(
				rpool, epool, qlen,
				newDepth0, newDepth1, newDepth2, newDepth3,
				newRdepth, 0, cost_, ham_ + ranges_[pos].eq.join.qual,
				top, bot, ep, ebwt, &edits_);
		// Add the new edit
		newBranch->edits_.add(e, epool, qlen);
		if(numNotEliminated == 1 && last) {
			// This branch is totally exhausted; there are no more
			// valid outgoing paths from any positions within it.
			// Remove it from the PathManager and mark it as exhausted.
			// The caller should delete it.
			exhausted_ = true;
		} else if(numTiedPositions == 1 && last) {
			// We exhausted the last outgoing edge at the current best
			// cost; update the best cost to be the next-best
			assert_neq(0xffff, nextCost);
			if(bestCost != nextCost) {
				cost_ -= bestCost;
				cost_ += nextCost;
			}
		}
		return newBranch;
	}

	/**
	 * Finalize an exhausted branch.
	 */
	void finalize(RangeStatePool& rpool,
	              AllocOnlyPool<Edit>& epool,
	              AllocOnlyPool<Branch>& bpool)
	{
		assert(exhausted_);
		if(lastBestCost_ == 0) {
			rpool.rewind(lastRpool_);
			epool.rewind(lastEpool_);
			bpool.rewind(lastBpool_);
		}
	}

	/**
	 * Pretty-print the state of this branch.
	 */
	void print(const String<Dna5>& qry,
	           const String<char>& quals,
	           uint16_t minCost,
	           std::ostream& out,
	           bool halfAndHalf,
	           bool seeded,
	           bool fw,
	           bool ebwtFw)
	{
		size_t editidx = 0;
		size_t printed = 0;
		const size_t qlen = seqan::length(qry);
		if(exhausted_)      out << "E ";
		else if(curtailed_) out << "C ";
		else                out << "  ";
		if(ebwtFw) out << "<";
		else       out << ">";
		if(fw)     out << "F ";
		else       out << "R ";
		std::stringstream ss;
		ss << cost_;
		string s = ss.str();
		if(s.length() < 6) {
			for(size_t i = 0; i < 6 - s.length(); i++) {
				out << "0";
			}
		}
		out << s << " ";
		std::stringstream ss2;
		ss2 << minCost;
		s = ss2.str();
		if(s.length() < 6) {
			for(size_t i = 0; i < 6 - s.length(); i++) {
				out << "0";
			}
		}
		out << s;
		if(halfAndHalf) out << " h";
		else if(seeded) out << " s";
		else            out << "  ";
		std::stringstream ss3;
		const size_t numEdits = edits_.size();
		if(rdepth_ > 0) {
			for(size_t i = 0; i < rdepth_; i++) {
				if(editidx < numEdits && edits_.get(editidx).pos == i) {
					ss3 << " " << "acgt"[edits_.get(editidx).chr];
					editidx++;
				} else {
					ss3 << " " << qry[qlen - i - 1];
				}
				printed++;
			}
			ss3 << "|";
		} else {
			ss3 << " ";
		}
		for(size_t i = 0; i < len_; i++) {
			if(editidx < numEdits && edits_.get(editidx).pos == printed) {
				ss3 << "acgt"[edits_.get(editidx).chr] << " ";
				editidx++;
			} else {
				ss3 << qry[qlen - printed - 1] << " ";
			}
			printed++;
		}
		assert_eq(editidx, edits_.size());
		for(size_t i = printed; i < qlen; i++) {
			ss3 << "- ";
		}
		s = ss3.str();
		if(ebwtFw) {
			std::reverse(s.begin(), s.end());
		}
		out << s << endl;
	}

	/**
	 * Called when the most recent branch extension resulted in an
	 * empty range or some other constraint violation (e.g., a
	 * half-and-half constraint).
	 */
	void curtail(RangeStatePool& rpool, int seedLen, bool qualOrder) {
		if(ranges_ == NULL) {
			exhausted_ = true;
			curtailed_ = true;
			return;
		}
		uint16_t lowestCost = 0xffff;
		// Iterate over positions in the path looking for the cost of
		// the lowest-cost non-eliminated position
		uint32_t eliminatedStretch = 0;
		int i = (int)depth0_;
		i = max(0, i - rdepth_);
		for(; i <= len_; i++) {
			if(!eliminated(i)) {
				eliminatedStretch = 0;
				uint16_t stratum = (rdepth_ + i < seedLen) ? (1 << 14) : 0;
				uint16_t cost = (qualOrder ? ranges_[i].eq.join.qual : 0) | stratum;
				if(cost < lowestCost) lowestCost = cost;
			} else {
				eliminatedStretch++;
			}
		}
		if(lowestCost > 0 && lowestCost != 0xffff) {
			// This branch's cost will change when curtailed; the
			// caller should re-insert it into the priority queue so
			// that the new cost takes effect.
			cost_ += lowestCost;
		} else if(lowestCost == 0xffff) {
			// This branch is totally exhausted; there are no more
			// valid outgoing paths from any positions within it.
			// Remove it from the PathManager and mark it as exhausted.
			// The caller should delete it.
			exhausted_ = true;
		} else {
			// Just mark it as curtailed and keep the same cost
		}
		curtailed_ = true;
	}

	/**
	 * Prep this branch for the next extension by calculating the
	 * SideLocus information and prefetching cache lines from the
	 * appropriate loci.
	 */
	void prep(const EbwtParams& ep, const uint8_t* ebwt) {
		if(bot_ > top_+1) {
			SideLocus::initFromTopBot(top_, bot_, ep, ebwt, ltop_, lbot_);
		} else if(bot_ > top_) {
			ltop_.initFromRow(top_, ep, ebwt);
			lbot_.invalidate();
		}
		prepped_ = true;
	}

	/**
	 * Get the furthest-out RangeState.
	 */
	RangeState* rangeState() {
		assert(ranges_ != NULL);
		return &ranges_[len_];
	}

	/**
	 * Set the elims to match the ranges in ranges_[len_], already
	 * calculated by the caller.  Only does mismatches for now.
	 */
	int installRanges(int c, int nextc, uint8_t q) {
		assert(ranges_ != NULL);
		RangeState& r = ranges_[len_];
		int ret = 0;
		r.eliminated_ = true; // start with everything eliminated
		r.eq.eliminate();
		r.eq.flags.qual = q;
		// Set one/both of these to true to do the accounting for
		// insertions and deletions as well as mismatches
		bool doInserts = false;
		bool doDeletes = false;
		// We can proceed on an A
		if(c != 0 && r.bots[0] > r.tops[0]) {
			r.eliminated_ = false;
			r.eq.flags.mmA = 0; // A substitution is an option
			if(doInserts) r.eq.flags.insA = 0;
			if(doDeletes && nextc == 0) r.eq.flags.del = 0;
			ret++;
		}
		// We can proceed on a C
		if(c != 1 && r.bots[1] > r.tops[1]) {
			r.eliminated_ = false;
			r.eq.flags.mmC = 0; // C substitution is an option
			if(doInserts) r.eq.flags.insC = 0;
			if(doDeletes && nextc == 1) r.eq.flags.del = 0;
			ret++;
		}
		// We can proceed on a G
		if(c != 2 && r.bots[2] > r.tops[2]) {
			r.eliminated_ = false;
			r.eq.flags.mmG = 0; // G substitution is an option
			if(doInserts) r.eq.flags.insG = 0;
			if(doDeletes && nextc == 2) r.eq.flags.del = 0;
			ret++;
		}
		// We can proceed on a T
		if(c != 3 && r.bots[3] > r.tops[3]) {
			r.eliminated_ = false;
			r.eq.flags.mmT = 0; // T substitution is an option
			if(doInserts) r.eq.flags.insT = 0;
			if(doDeletes && nextc == 3) r.eq.flags.del = 0;
			ret++;
		}
		return ret;
	}

	/**
	 * Extend this branch by one position.
	 */
	void extend() {
		assert(ranges_ != NULL);
		assert(repOk());
		prepped_ = false;
		len_++;
	}

	/**
	 * Do an internal consistency check
	 */
	bool repOk(uint32_t qlen = 0) const{
		assert_leq(depth0_, depth1_);
		assert_leq(depth1_, depth2_);
		assert_leq(depth2_, depth3_);
		if(qlen > 0) {
			assert_leq(edits_.size(), qlen); // might have to relax this with inserts
			assert_leq(rdepth_, qlen);
		}
		for(int i = 0; i < len_; i++) {
			if(!eliminated(i)) {
				assert_lt(i, (int)(len_));
				assert(ranges_[i].repOk());
			}
		}
		const size_t numEdits = edits_.size();
		for(size_t i = 0; i < numEdits; i++) {
			for(size_t j = i+1; j < numEdits; j++) {
				// No two edits should be at the same position (might
				// have to relax this with inserts)
				assert_neq(edits_.get(i).pos, edits_.get(j).pos);
			}
		}
		assert_lt((cost_ >> 14), 4);
		return true;
	}

	uint16_t depth0_; // no edits at depths < depth0
	uint16_t depth1_; // at most 1 edit at depths < depth1
	uint16_t depth2_; // at most 2 edits at depths < depth2
	uint16_t depth3_; // at most 3 edits at depths < depth3

	uint16_t rdepth_; // offset in read space from root of search space
	uint16_t len_;    // length of the branch
	uint16_t cost_;   // top 2 bits = stratum, bottom 14 = qual ham
	                  // it's up to Branch to keep this updated with the
	                  // cumulative cost of the best path leaving the
	                  // branch; if the branch hasn't been fully
	                  // extended yet, then that path will always be the
	                  // one that extends it by one more
	uint16_t ham_;    // quality-weighted hamming distance so far
	RangeState *ranges_; // Allocated from the RangeStatePool
	uint32_t top_;    // top offset leading to the root of this subtree
	uint32_t bot_;    // bot offset leading to the root of this subtree
	SideLocus ltop_;
	SideLocus lbot_;
	EditList edits_;   // edits leading to the root of the branch
	uint16_t lastBestCost_; // cost of the last split-off branch;
	                        // 0xffff if there haven't been splits yet
	U32Pair lastRpool_;
	U32Pair lastEpool_;
	U32Pair lastBpool_;

	bool curtailed_;  // can't be extended anymore without using edits
	bool exhausted_;  // all outgoing edges exhausted, including all edits
	bool prepped_;    // whether SideLocus's are inited

protected:

};

/**
 * Order two Branches based on cost.
 */
class CostCompare {
public:
	/**
	 * true -> b before a
	 * false -> a before b
	 */
	bool operator()(const Branch* a, const Branch* b) const {
		// Branch with the best cost
		if(a->cost_ == b->cost_) {
			// If one or the other is curtailed, take the one that's
			// still getting extended
			if(b->curtailed_ && !a->curtailed_) {
				// a still being extended, return false
				return false;
			}
			if(a->curtailed_ && !b->curtailed_) {
				// b still being extended, return true
				return true;
			}
			// Either both are curtailed or both are still being
			// extended, pick based on which one is deeper
			if(a->tipDepth() == b->tipDepth()) {
				return false;
			}
			else {
				// Expression is true if b is deeper
				return a->tipDepth() < b->tipDepth();
			}
		} else {
			return b->cost_ < a->cost_;
		}
	}

	static bool equal(const Branch* a, const Branch* b) {
		return a->cost_ == b->cost_ && a->curtailed_ == b->curtailed_ && a->tipDepth() == b->tipDepth();
	}
};

#if 1
/**
 * A priority queue for Branch objects; makes it easy to process
 * branches in a best-first manner by prioritizing branches with lower
 * cumulative costs over branches with higher cumulative costs.
 */
class BranchQueue {

	typedef std::pair<int, int> TIntPair;
	typedef std::priority_queue<Branch*, std::vector<Branch*>, CostCompare> TBranchQueue;

public:

	BranchQueue() : sz_(0), branchQ_() { }

	/**
	 * Return the front (highest-priority) element of the queue.
	 */
	Branch *front() {
		return branchQ_.top();
	}

	/**
	 * Remove and return the front (highest-priority) element of the
	 * queue.
	 */
	Branch *pop() {
		Branch *b = branchQ_.top(); // get it
		branchQ_.pop(); // remove it
		sz_--;
		return b;
	}

	/**
	 * Insert a new Branch into the sorted priority queue.
	 */
	void push(Branch *b) {
#ifndef NDEBUG
		bool bIsBetter = empty() || !CostCompare()(b, branchQ_.top());
#endif
		branchQ_.push(b);
#ifndef NDEBUG
		assert(bIsBetter  || branchQ_.top() != b || CostCompare::equal(branchQ_.top(), b));
		assert(!bIsBetter || branchQ_.top() == b || CostCompare::equal(branchQ_.top(), b));
#endif
		sz_++;
	}

	/**
	 * Empty the priority queue and reset the count.
	 */
	void reset() {
		while(!branchQ_.empty()) {
			branchQ_.pop();
			assert_gt(sz_, 0);
			sz_--;
		}
		assert_eq(0, sz_);
	}

	/**
	 * Return true iff the priority queue of branches is empty.
	 */
	bool empty() const {
		bool ret = branchQ_.empty();
		assert(ret || sz_ > 0);
		assert(!ret || sz_ == 0);
		return ret;
	}

	/**
	 * Return the number of Branches in the queue.
	 */
	uint32_t size() const {
		return sz_;
	}

#ifndef NDEBUG
	/**
	 * Consistency check.
	 */
	bool repOk(std::set<Branch*>& bset) {
		TIntPair pair = bestStratumAndHam(bset);
		Branch *b = branchQ_.top();
		assert_eq(pair.first, (b->cost_ >> 14));
		assert_eq(pair.second, (b->cost_ & ~0xc000));
		return true;
	}
#endif

protected:

#ifndef NDEBUG
	/**
	 * Return the stratum and quality-weight (sum of qualities of all
	 * edited positions) of the lowest-cost branch.
	 */
	TIntPair bestStratumAndHam(std::set<Branch*>& bset) const {
		TIntPair ret = make_pair(0xffff, 0xffff);
		std::set<Branch*>::iterator it;
		for(it = bset.begin(); it != bset.end(); it++) {
			Branch *b = *it;
			int stratum = b->cost_ >> 14;
			assert_lt(stratum, 4);
			int qual = b->cost_ & ~0xc000;
			if(stratum < ret.first ||
			   (stratum == ret.first && qual < ret.second))
			{
				ret.first = stratum;
				ret.second = qual;
			}
		}
		return ret;
	}
#endif

	uint32_t sz_;
	TBranchQueue branchQ_; // priority queue of branches
};
#else
/**
 * A priority queue for Branch objects; makes it easy to process
 * branches in a best-first manner by prioritizing branches with lower
 * cumulative costs over branches with higher cumulative costs.
 */
class BranchQueue {

	typedef std::pair<int, int> TIntPair;

public:

	BranchQueue() : branchQ_() { }

	/**
	 * Return the front (highest-priority) element of the queue.
	 */
	Branch *front() {
		assert(!branchQ_.empty());
		return branchQ_.back();
	}

	/**
	 * Remove and return the front (highest-priority) element of the
	 * queue.
	 */
	Branch *pop() {
		assert(!branchQ_.empty());
		Branch *b = branchQ_.back();
		branchQ_.pop_back();
		return b;
	}

	/**
	 * Insert a new Branch into the sorted priority queue.
	 */
	void push(Branch *b) {
		branchQ_.push_back(b);
	}

	/**
	 * Empty the priority queue and reset the count.
	 */
	void reset() {
		branchQ_.clear();
	}

	/**
	 * Return true iff the priority queue of branches is empty.
	 */
	bool empty() const {
		return branchQ_.empty();
	}

	/**
	 * Return the number of Branches in the queue.
	 */
	uint32_t size() const {
		return branchQ_.size();
	}

#ifndef NDEBUG
	/**
	 * Consistency check.
	 */
	bool repOk(std::set<Branch*>& bset) {
		return true;
	}
#endif

protected:

	std::vector<Branch*> branchQ_; // priority queue of branches
};
#endif

/**
 * A class that both contains Branches and determines how those
 * branches are extended to form longer paths.  The overall goal is to
 * find the best full alignment(s) as quickly as possible so that a
 * successful search can finish quickly.  A second goal is to ensure
 * that the most "promising" paths are tried first so that, if there is
 * a limit on the amount of effort spent searching before we give up,
 * we can be as sensitive as possible given that limit.
 *
 * The quality (or cost) of a given alignment path will ultimately be
 * configurable.  The default cost model is:
 *
 * 1. Mismatches incur one "edit" penalty and a "quality" penalty with
 *    magnitude equal to the Phred quality score of the read position
 *    involved in the edit (note that insertions into the read are a
 *    little trickier).
 * 2. Edit penalties are all more costly than any quality penalty; i.e.
 *    the policy sorts alignments first by edit penalty, then by
 *    quality penalty.
 * 3. For the Maq-like alignment policy, edit penalties saturate (don't
 *    get any greater) after leaving the seed region of the alignment.
 */
class PathManager {

public:

	PathManager(AllocOnlyPool<Branch> *bpool_,
	            RangeStatePool *rpool_,
	            AllocOnlyPool<Edit> *epool_,
	            int *btCnt = NULL) :
	            branchQ_(),
	            bpool(bpool_), rpool(rpool_), epool(epool_),
	            minCost(0), btCnt_(btCnt)
	{
		assert(bpool != NULL);
		assert(rpool != NULL);
		assert(epool != NULL);
	}

	~PathManager() {
		// All the RangeState's and Branch's are dropped at this point
		// as their respective Pools are dropped
	}

	/**
	 * Return the "front" (highest-priority) branch in the collection.
	 */
	Branch* front() {
		assert(!empty());
		return branchQ_.front();
	}

	/**
	 * Pop the highest-priority (lowest cost) element from the
	 * priority queue.
	 */
	Branch* pop() {
		Branch* b = branchQ_.pop();
#ifndef NDEBUG
		// Also remove it from the set
		assert(branchSet_.find(b) != branchSet_.end());
		branchSet_.erase(branchSet_.find(b));
		if(!branchQ_.empty()) {
			// Top shouldn't be b any more
			Branch *newtop = branchQ_.front();
			assert(b != newtop);
		}
#endif
		// Update this PathManager's cost
		minCost = branchQ_.front()->cost_;
		assert(repOk());
		return b;
	}

	/**
	 * Push a new element onto the priority queue.
	 */
	void push(Branch *b) {
		branchQ_.push(b);
#ifndef NDEBUG
		// Also insert it into the set
		assert(branchSet_.find(b) == branchSet_.end());
		branchSet_.insert(b);
#endif
		// Update this PathManager's cost
		minCost = branchQ_.front()->cost_;
	}

	/**
	 * Reset the PathManager, clearing out the priority queue and
	 * resetting the RangeStatePool.
	 */
	void reset() {
		branchQ_.reset();
		ASSERT_ONLY(branchSet_.clear());
		// These are now reset at the Aligner level
//		assert(rpool != NULL);
//		assert(bpool != NULL);
//		assert(epool != NULL);
//		rpool->reset();
//		bpool->reset();
//		epool->reset();
		minCost = 0;
	}

#ifndef NDEBUG
	/**
	 * Return true iff Branch b is in the priority queue;
	 */
	bool contains(Branch *b) const {
		bool ret = branchSet_.find(b) != branchSet_.end();
		assert(!ret || !b->exhausted_);
		return ret;
	}

	/**
	 * Do a consistenty-check on the collection of branches contained
	 * in this PathManager.
	 */
	bool repOk() {
		if(empty()) return true;
		assert(branchQ_.repOk(branchSet_));
		return true;
	}
#endif

	/**
	 * Return true iff the priority queue of branches is empty.
	 */
	bool empty() const {
		bool ret = branchQ_.empty();
		assert_eq(ret, branchSet_.empty());
		return ret;
	}

	/**
	 * Curtail the given branch, and possibly remove it from or
	 * re-insert it into the priority queue.
	 */
	void curtail(Branch *br, int seedLen, bool qualOrder) {
		assert(!br->exhausted_);
		assert(!br->curtailed_);
		uint16_t origCost = br->cost_;
		br->curtail(*rpool, seedLen, qualOrder);
		assert(br->curtailed_);
		assert_geq(br->cost_, origCost);
		if(br->exhausted_) {
			assert(br == front());
			ASSERT_ONLY(Branch *popped =) pop();
			assert(popped == br);
			// br is allocated from a pool; it'll get freed later
		} else if(br->cost_ != origCost) {
			assert(br == front());
			Branch *popped = pop();
			assert(popped == br);
			push(popped);
		}
	}

	/**
	 * If the frontmost branch is a curtailed branch, split off an
	 * extendable branch and add it to the queue.
	 */
	void splitAndPrep(RandomSource& rand, uint32_t qlen, int seedLen,
	                  bool qualOrder,
	                  const EbwtParams& ep, const uint8_t* ebwt)
	{
		if(empty()) return;
		// This counts as a backtrack
		if(btCnt_ != NULL && (*btCnt_ == 0)) {
			// Abruptly end the search by removing all possibilities
			branchQ_.reset();
			ASSERT_ONLY(branchSet_.clear());
			assert(empty());
			return;
		}
		Branch *f = front();
		while(f->exhausted_) {
			f->finalize(*rpool, *epool, *bpool);
			pop();
			if(empty()) return;
			f = front();
		}
		if(f->curtailed_) {
			uint16_t origCost = f->cost_;
			// This counts as a backtrack
			if(btCnt_ != NULL) {
				if(--(*btCnt_) == 0) {
					// Abruptly end the search by removing all
					// possibilities
					branchQ_.reset();
					ASSERT_ONLY(branchSet_.clear());
					assert(empty());
					return;
				}
			}
			Branch* newbr = splitBranch(f, rand, qlen, seedLen,
			                            qualOrder, ep, ebwt);
			if(f->cost_ > origCost) {
				// br's cost changed so we need to re-insert it into
				// the priority queue
				Branch *popped = pop();
				assert(popped == f); // should be br!
				push(popped); // re-insert
			}
			assert(newbr != NULL);
			push(newbr);
			assert(newbr == front());
		}
		prep(ep, ebwt);
	}

	/**
	 * Return true iff the front element of the queue is prepped.
	 */
	bool prepped() {
		return front()->prepped_;
	}

protected:

	/**
	 * Split off an extendable branch from a curtailed branch.
	 */
	Branch* splitBranch(Branch* src, RandomSource& rand, uint32_t qlen,
	                    int seedLen, bool qualOrder,
	                    const EbwtParams& ep, const uint8_t* ebwt)
	{
		Branch *dst = bpool->alloc();
		src->splitBranch(*rpool, *epool, *bpool, dst, rand, qlen,
		                 seedLen, qualOrder, ep, ebwt);
		assert(dst->repOk());
		return dst;
	}

	/**
	 * Prep the next branch to be extended in advanceBranch().
	 */
	void prep(const EbwtParams& ep, const uint8_t* ebwt) {
		if(!branchQ_.empty()) {
			branchQ_.front()->prep(ep, ebwt);
		}
	}

	BranchQueue branchQ_; // priority queue for selecting lowest-cost Branch
	// set of branches in priority queue, for sanity checks
	ASSERT_ONLY(std::set<Branch*> branchSet_);

public:

	AllocOnlyPool<Branch> *bpool; // pool for allocating Branches
	RangeStatePool *rpool; // pool for allocating RangeStates
	AllocOnlyPool<Edit> *epool; // pool for allocating Edits
	/// The minimum possible cost for any alignments obtained by
	/// advancing further
	uint16_t minCost;

protected:
	/// Pointer to the aligner's per-read backtrack counter.  We
	/// increment it in splitBranch.
	int *btCnt_;
};

/**
 * Encapsulates an algorithm that navigates the Bowtie index to produce
 * candidate ranges of alignments in the Burrows-Wheeler matrix.  A
 * higher authority is responsible for reporting hits out of those
 * ranges, and stopping when the consumer is satisfied.
 */
class RangeSource {
	typedef Ebwt<String<Dna> > TEbwt;
public:
	RangeSource() :
		done(false), foundRange(false), curEbwt_(NULL) { }

	virtual ~RangeSource() { }

	/// Set query to find ranges for
	virtual void setQuery(ReadBuf& r, Range *partial) = 0;
	/// Set up the range search.
	virtual void initBranch(PathManager& pm) = 0;
	/// Advance the range search by one memory op
	virtual void advanceBranch(int until, uint16_t minCost, PathManager& pm) = 0;

	/// Return the last valid range found
	virtual Range& range() = 0;
	/// Return ptr to index this RangeSource is currently getting ranges from
	const TEbwt *curEbwt() const { return curEbwt_; }

	/// All searching w/r/t the current query is finished
	bool done;
	/// Set to true iff the last call to advance yielded a range
	bool foundRange;
protected:
	/// ptr to index this RangeSource is currently getting ranges from
	const TEbwt *curEbwt_;
};

/**
 * Abstract parent of RangeSourceDrivers.
 */
template<typename TRangeSource>
class RangeSourceDriver {
	typedef Ebwt<String<Dna> > TEbwt;
public:
	RangeSourceDriver(bool _done, uint32_t minCostAdjustment = 0) :
		foundRange(false), done(_done), minCostAdjustment_(minCostAdjustment)
	{
		minCost = minCostAdjustment_;
	}

	virtual ~RangeSourceDriver() { }

	/**
	 * Prepare this aligner for the next read.
	 */
	virtual void setQuery(PatternSourcePerThread* patsrc, Range *r) {
		// Clear our buffer of previously-dished-out top offsets
		ASSERT_ONLY(allTops_.clear());
		setQueryImpl(patsrc, r);
	}

	/**
	 * Prepare this aligner for the next read.
	 */
	virtual void setQueryImpl(PatternSourcePerThread* patsrc, Range *r) = 0;

	/**
	 * Advance the aligner by one memory op.  Return true iff we're
	 * done with this read.
	 */
	virtual void advance(int until) {
		advanceImpl(until);
#ifndef NDEBUG
		if(this->foundRange) {
			// Assert that we have not yet dished out a range with this
			// top offset
			assert_gt(range().bot, range().top);
			assert(range().ebwt != NULL);
			int64_t top = (int64_t)range().top;
			top++; // ensure it's not 0
			if(!range().ebwt->fw()) top = -top;
			assert(allTops_.find(top) == allTops_.end());
			allTops_.insert(top);
		}
#endif
	}
	/**
	 * Advance the aligner by one memory op.  Return true iff we're
	 * done with this read.
	 */
	virtual void advanceImpl(int until) = 0;
	/**
	 * Return the range found.
	 */
	virtual Range& range() = 0;

	/**
	 * Return whether we're generating ranges for the first or the
	 * second mate.
	 */
	virtual bool mate1() const = 0;

	/**
	 * Return true iff current pattern is forward-oriented.
	 */
	virtual bool fw() const = 0;

	/// Set to true iff we just found a range.
	bool foundRange;

	/**
	 * Set to true if all searching w/r/t the current query is
	 * finished or if there is no current query.
	 */
	bool done;

	/**
	 * The minimum "combined" stratum/qual cost that could possibly
	 * result from subsequent calls to advance() for this driver.
	 */
	uint16_t minCost;

	/**
	 * Adjustment to the minCost given by the caller that constructed
	 * the object.  This is useful if we know the lowest-cost branch is
	 * likely to cost less than the any of the alignments that could
	 * possibly result from advancing (e.g. when we're going to force a
	 * mismatch somewhere down the line).
	 */
	uint16_t minCostAdjustment_;

protected:

#ifndef NDEBUG
	std::set<int64_t> allTops_;
#endif
};

/**
 * A concrete driver wrapper for a single RangeSource.
 */
template<typename TRangeSource>
class SingleRangeSourceDriver : public RangeSourceDriver<TRangeSource> {

	typedef Ebwt<String<Dna> > TEbwt;

public:
	SingleRangeSourceDriver(
		EbwtSearchParams<String<Dna> >& params,
		TRangeSource* rs,
		bool fw,
		HitSink& sink,
		HitSinkPerThread* sinkPt,
		vector<String<Dna5> >& os,
		bool verbose,
		bool mate1,
		uint32_t minCostAdjustment,
		AllocOnlyPool<Branch>* bpool,
		RangeStatePool* rpool,
		AllocOnlyPool<Edit>* epool,
		int *btCnt) :
		RangeSourceDriver<TRangeSource>(true, minCostAdjustment),
		len_(0), mate1_(mate1),
		sinkPt_(sinkPt),
		params_(params),
		fw_(fw), rs_(rs),
		ebwtFw_(rs_->curEbwt()->fw()),
		pm_(bpool, rpool, epool, btCnt)
	{
		assert(rs_ != NULL);
	}

	virtual ~SingleRangeSourceDriver() {
		delete rs_; rs_ = NULL;
	}

	/**
	 * Prepare this aligner for the next read.
	 */
	virtual void setQueryImpl(PatternSourcePerThread* patsrc, Range *r) {
		this->done = false;
		pm_.reset();
		if(mate1_) {
			ReadBuf& buf = patsrc->bufa();
			len_ = buf.length();
			rs_->setQuery(buf, r);
			initRangeSource((fw_ == ebwtFw_) ? buf.qualFw : buf.qualRc);
		} else {
			ReadBuf& buf = patsrc->bufb();
			len_ = buf.length();
			rs_->setQuery(buf, r);
			initRangeSource((fw_ == ebwtFw_) ? buf.qualFw : buf.qualRc);
		}
		assert_gt(len_, 0);
		if(this->done) return;
		ASSERT_ONLY(allTops_.clear());
		if(!rs_->done) {
			rs_->initBranch(pm_); // set up initial branch
		}
		uint16_t icost = (r != NULL) ? r->cost : 0;
		this->minCost = max<uint16_t>(icost, this->minCostAdjustment_);
		this->done = rs_->done;
		this->foundRange = rs_->foundRange;
		if(!pm_.empty()) {
			assert(!pm_.front()->curtailed_);
			assert(!pm_.front()->exhausted_);
		}
	}

	/**
	 * Advance the aligner by one memory op.  Return true iff we're
	 * done with this read.
	 */
	virtual void advanceImpl(int until) {
		if(this->done || pm_.empty()) {
			this->done = true;
			return;
		}
		assert(!pm_.empty());
		assert(!pm_.front()->curtailed_);
		assert(!pm_.front()->exhausted_);
		params_.setFw(fw_);
		// Advance the RangeSource for the forward-oriented read
		ASSERT_ONLY(uint16_t oldMinCost = this->minCost);
		rs_->advanceBranch(until, this->minCost, pm_);
		this->minCost = max(pm_.minCost, this->minCostAdjustment_);
		assert_geq(this->minCost, oldMinCost);
		this->done = pm_.empty();
		this->foundRange = rs_->foundRange;
#ifndef NDEBUG
		if(this->foundRange) {
			if(until >= ADV_COST_CHANGES) assert_eq(oldMinCost, range().cost);
			// Assert that we have not yet dished out a range with this
			// top offset
			assert_gt(range().bot, range().top);
			assert(range().ebwt != NULL);
			int64_t top = (int64_t)range().top;
			top++; // ensure it's not 0
			if(!range().ebwt->fw()) top = -top;
			assert(allTops_.find(top) == allTops_.end());
			allTops_.insert(top);
		}
		if(!pm_.empty()) {
			assert(!pm_.front()->curtailed_);
			assert(!pm_.front()->exhausted_);
		}
#endif
	}

	/**
	 * Return the range found.
	 */
	virtual Range& range() {
		rs_->range().fw = fw_;
		rs_->range().mate1 = mate1_;
		return rs_->range();
	}

	/**
	 * Return whether we're generating ranges for the first or the
	 * second mate.
	 */
	bool mate1() const {
		return mate1_;
	}

	/**
	 * Return true iff current pattern is forward-oriented.
	 */
	bool fw() const {
		return fw_;
	}

	virtual void initRangeSource(const String<char>& qual) = 0;

protected:

	// Progress state
	uint32_t len_;
	bool mate1_;

	// Temporary HitSink; to be deleted
	HitSinkPerThread* sinkPt_;

	// State for alignment
	EbwtSearchParams<String<Dna> >& params_;
	bool                            fw_;
	TRangeSource*                   rs_; // delete this in destructor
	bool ebwtFw_;
	PathManager pm_;
	ASSERT_ONLY(std::set<int64_t> allTops_);
};

/**
 * Encapsulates an algorithm that navigates the Bowtie index to produce
 * candidate ranges of alignments in the Burrows-Wheeler matrix.  A
 * higher authority is responsible for reporting hits out of those
 * ranges, and stopping when the consumer is satisfied.
 */
template<typename TRangeSource>
class ListRangeSourceDriver : public RangeSourceDriver<TRangeSource> {

	typedef Ebwt<String<Dna> > TEbwt;
	typedef std::vector<RangeSourceDriver<TRangeSource>*> TRangeSrcDrPtrVec;

public:

	ListRangeSourceDriver(const TRangeSrcDrPtrVec& rss) :
		RangeSourceDriver<TRangeSource>(false),
		cur_(0), ham_(0), rss_(rss) /* copy */,
		patsrc_(NULL), seedRange_(NULL)
	{
		assert_gt(rss_.size(), 0);
		assert(!this->done);
	}

	virtual ~ListRangeSourceDriver() {
		for(size_t i = 0; i < rss_.size(); i++) {
			delete rss_[i];
		}
	}

	/// Set query to find ranges for
	virtual void setQueryImpl(PatternSourcePerThread* patsrc, Range *r) {
		cur_ = 0; // go back to first RangeSource in list
		this->done = false;
		rss_[cur_]->setQuery(patsrc, r);
		patsrc_ = patsrc; // so that we can call setQuery on the other elements later
		seedRange_ = r;
		this->done = (cur_ == rss_.size()-1) && rss_[cur_]->done;
		this->minCost = max(rss_[cur_]->minCost, this->minCostAdjustment_);
		this->foundRange = rss_[cur_]->foundRange;
	}

	/// Advance the range search by one memory op
	virtual void advanceImpl(int until) {
		assert(!this->done);
		assert_lt(cur_, rss_.size());
		if(rss_[cur_]->done) {
			// Move on to next RangeSourceDriver
			if(cur_ < rss_.size()-1) {
				rss_[++cur_]->setQuery(patsrc_, seedRange_);
				this->minCost = max(rss_[cur_]->minCost, this->minCostAdjustment_);
				this->foundRange = rss_[cur_]->foundRange;
			} else {
				// No RangeSources in list; done
				cur_ = 0xffffffff;
				this->done = true;
			}
		} else {
			// Advance current RangeSource
			rss_[cur_]->advance(until);
			this->done = (cur_ == rss_.size()-1 && rss_[cur_]->done);
			this->foundRange = rss_[cur_]->foundRange;
			this->minCost = max(rss_[cur_]->minCost, this->minCostAdjustment_);
		}
	}

	/// Return the last valid range found
	virtual Range& range() { return rss_[cur_]->range(); }

	/**
	 * Return whether we're generating ranges for the first or the
	 * second mate.
	 */
	virtual bool mate1() const {
		return rss_[0]->mate1();
	}

	/**
	 * Return true iff current pattern is forward-oriented.
	 */
	virtual bool fw() const {
		return rss_[0]->fw();
	}

protected:

	uint32_t cur_;
	uint32_t ham_;
	TRangeSrcDrPtrVec rss_;
	PatternSourcePerThread* patsrc_;
	Range *seedRange_;
};

/**
 * A RangeSourceDriver that wraps a set of other RangeSourceDrivers and
 * chooses which one to advance at any given moment by picking one with
 * minimal "cumulative cost" so far.
 *
 * Note that costs have to be "adjusted" to account for the fact that
 * the alignment policy for the underlying RangeSource might force
 * mismatches.
 */
template<typename TRangeSource>
class CostAwareRangeSourceDriver : public RangeSourceDriver<TRangeSource> {

	typedef Ebwt<String<Dna> > TEbwt;
	typedef RangeSourceDriver<TRangeSource>* TRangeSrcDrPtr;
	typedef std::vector<TRangeSrcDrPtr> TRangeSrcDrPtrVec;

public:

	CostAwareRangeSourceDriver(
			bool strandFix,
			const TRangeSrcDrPtrVec* rss,
			bool verbose,
			bool deleteOnDone = false) :
		RangeSourceDriver<TRangeSource>(false),
		rss_(), active_(), strandFix_(strandFix),
		lastRange_(NULL), delayedRange_(NULL), patsrc_(NULL),
		verbose_(verbose)
	{
		if(rss != NULL) {
			rss_ = (*rss);
		}
		paired_ = false;
		this->foundRange = false;
		this->done = false;
		if(rss_.empty()) {
			return;
		}
		calcPaired();
		active_ = rss_;
		this->minCost = 0;
	}

	/// Destroy all underlying RangeSourceDrivers
	virtual ~CostAwareRangeSourceDriver() {
		const size_t rssSz = rss_.size();
		for(size_t i = 0; i < rssSz; i++) {
			delete rss_[i];
		}
		rss_.clear();
		active_.clear();
	}

	/// Set query to find ranges for
	virtual void setQueryImpl(PatternSourcePerThread* patsrc, Range *r) {
		this->done = false;
		this->foundRange = false;
		lastRange_ = NULL;
		delayedRange_ = NULL;
		ASSERT_ONLY(allTopsRc_.clear());
		patsrc_ = patsrc;
		rand_.init(patsrc->bufa().seed);
		const size_t rssSz = rss_.size();
		if(rssSz == 0) return;
		for(size_t i = 0; i < rssSz; i++) {
			// Assuming that all
			rss_[i]->setQuery(patsrc, r);
		}
		active_ = rss_;
		this->minCost = 0;
		sortActives();
	}

	/**
	 * Add a new RangeSource to the list and re-sort it.
	 */
	void addSource(TRangeSrcDrPtr p, Range *r) {
		assert(!this->foundRange);
		this->lastRange_ = NULL;
		this->delayedRange_ = NULL;
		this->done = false;
		if(patsrc_ != NULL) {
			p->setQuery(patsrc_, r);
		}
		rss_.push_back(p);
		active_.push_back(p);
		calcPaired();
		this->minCost = 0;
		sortActives();
	}

	/**
	 * Free and remove all contained RangeSources.
	 */
	void clearSources() {
		const size_t rssSz = rss_.size();
		for(size_t i = 0; i < rssSz; i++) {
			delete rss_[i];
		}
		rss_.clear();
		active_.clear();
		paired_ = false;
	}

	/**
	 * Return the number of RangeSources contained within.
	 */
	size_t size() const {
		return rss_.size();
	}

	/**
	 * Return true iff no RangeSources are contained within.
	 */
	bool empty() const {
		return rss_.empty();
	}

	/**
	 * Advance the aligner by one memory op.  Return true iff we're
	 * done with this read.
	 */
	virtual void advance(int until) {
		ASSERT_ONLY(uint16_t precost = this->minCost);
		assert(!this->done);
		assert(!this->foundRange);
		until = max<int>(until, ADV_COST_CHANGES);
		advanceImpl(until);
		assert(!this->foundRange || lastRange_ != NULL);
		if(this->foundRange) {
			assert_eq(range().cost, precost);
		}
	}

	/// Advance the range search
	virtual void advanceImpl(int until) {
		lastRange_ = NULL;
		ASSERT_ONLY(uint16_t iminCost = this->minCost);
		const size_t actSz = active_.size();
		assert(sortedActives());
		if(delayedRange_ != NULL) {
			assert_eq(iminCost, delayedRange_->cost);
			lastRange_ = delayedRange_;
			delayedRange_ = NULL;
			this->foundRange = true;
			assert_eq(range().cost, iminCost);
			if(!active_.empty()) {
				assert_geq(active_[0]->minCost, this->minCost);
				this->minCost = max(active_[0]->minCost, this->minCost);
			} else {
				this->done = true;
			}
			return; // found a range
		}
		assert(delayedRange_ == NULL);
		if(mateEliminated() || actSz == 0) {
			// No more alternatoves; clear the active set and signal
			// we're done
			active_.clear();
			this->done = true;
			return;
		}
		// Advance lowest-cost RangeSourceDriver
		TRangeSrcDrPtr p = active_[0];
		uint16_t precost = p->minCost;
		assert(!p->done || p->foundRange);
		if(!p->foundRange) {
			p->advance(until);
		}
		bool needsSort = false;
		if(p->foundRange) {
			Range *r = &p->range();
			assert_eq(r->cost, iminCost);
			needsSort = foundFirstRange(r); // may set delayedRange_; re-sorts active_
			assert_eq(lastRange_->cost, iminCost);
			if(delayedRange_ != NULL) assert_eq(delayedRange_->cost, iminCost);
			p->foundRange = false;
		}
		if(p->done || (precost != p->minCost) || needsSort) {
			sortActives();
			if(mateEliminated() || active_.empty()) {
				active_.clear();
				this->done = (delayedRange_ == NULL);
			}
		}
		assert(sortedActives());
		assert(lastRange_ == NULL || lastRange_->cost == iminCost);
		assert(delayedRange_ == NULL || delayedRange_->cost == iminCost);
	}

	/// Return the last valid range found
	virtual Range& range() {
		assert(lastRange_ != NULL);
		return *lastRange_;
	}

	/**
	 * Return whether we're generating ranges for the first or the
	 * second mate.
	 */
	virtual bool mate1() const {
		return rss_[0]->mate1();
	}

	/**
	 * Return true iff current pattern is forward-oriented.
	 */
	virtual bool fw() const {
		return rss_[0]->fw();
	}

protected:

	/**
	 * Set paired_ to true iff there are mate1 and mate2 range sources
	 * in the rss_ vector.
	 */
	void calcPaired() {
		const size_t rssSz = rss_.size();
		bool saw1 = false;
		bool saw2 = false;
		for(size_t i = 0; i < rssSz; i++) {
			if(rss_[i]->mate1()) saw1 = true;
			else saw2 = true;
		}
		assert(saw1 || saw2);
		paired_ = saw1 && saw2;
	}

	/**
	 * Return true iff one mate or the other has been eliminated.
	 */
	bool mateEliminated() {
		if(!paired_) return false;
		bool mate1sLeft = false;
		bool mate2sLeft = false;
		// If this RangeSourceDriver is done, shift everyone else
		// up and delete it
		const size_t rssSz = active_.size();
		for(size_t i = 0; i < rssSz-1; i++) {
			if(!active_[i]->done) {
				if(active_[i]->mate1()) mate1sLeft = true;
				else mate2sLeft = true;
			}
		}
		return !mate1sLeft || !mate2sLeft;
	}

#ifndef NDEBUG
	/**
	 * Check that the given range has not yet been reported.
	 */
	bool checkRange(Range* r) {
		// Assert that we have not yet dished out a range with this
		// top offset
		assert_gt(r->bot, r->top);
		assert(r->ebwt != NULL);
		int64_t top = (int64_t)r->top;
		top++; // ensure it's not 0
		if(!r->ebwt->fw()) top = -top;
		if(r->fw) {
			assert(this->allTops_.find(top) == this->allTops_.end());
			this->allTops_.insert(top);
		} else {
			assert(this->allTopsRc_.find(top) == this->allTopsRc_.end());
			this->allTopsRc_.insert(top);
		}
		return true;
	}
#endif

	/**
	 * We found a range; check whether we should attempt to find a
	 * range of equal quality from the opposite strand so that we can
	 * resolve the strand bias.  Return true iff we modified the cost
	 * of one or more items after the first item.
	 */
	bool foundFirstRange(Range* r) {
		assert(r != NULL);
		assert(checkRange(r));
		this->foundRange = true;
		lastRange_ = r;
		if(strandFix_) {
			// We found a range but there may be an equally good range
			// on the other strand; let's try to get it.
			const size_t sz = active_.size();
			for(size_t i = 1; i < sz; i++) {
				// Same mate, different orientation?
				if(rss_[i]->mate1() == r->mate1 && rss_[i]->fw() != r->fw) {
					// Yes; see if it has the same cost
					TRangeSrcDrPtr p = active_[i];
					uint16_t minCost = max(this->minCost, p->minCost);
					if(minCost > r->cost) break;
					// Yes, it has the same cost
					assert_eq(minCost, r->cost); // can't be better
					// Advance it until it's done, we've found a range,
					// or its cost increases
					if(this->verbose_) cout << " Looking for opposite range to avoid strand bias:" << endl;
					while(!p->done && !p->foundRange) {
						p->advance(ADV_COST_CHANGES);
						assert_geq(p->minCost, minCost);
						if(p->minCost > minCost) break;
					}
					if(p->foundRange) {
						// Found one!  Now we have to choose which one
						// to give out first; we choose randomly using
						// the size of the ranges as weights.
						delayedRange_ = &p->range();
						assert(checkRange(delayedRange_));
						size_t tot = (delayedRange_->bot - delayedRange_->top) +
						             (lastRange_->bot    - lastRange_->top);
						uint32_t rq = rand_.nextU32() % tot;
						// We picked this range, not the first one
						if(rq < (delayedRange_->bot - delayedRange_->top)) {
							Range *tmp = lastRange_;
							lastRange_ = delayedRange_;
							delayedRange_ = tmp;
						}
						p->foundRange = false;
					}
					// Return true iff we need to force a re-sort
					return true;
				}
			}
			// OK, now we have a choice of two equally good ranges from
			// each strand.
		}
		return false;
	}

	/**
	 * Sort all of the RangeSourceDriver ptrs in the rss_ array so that
	 * the one with the lowest cumulative cost is at the top.  Break
	 * ties randomly.  Just do selection sort for now; we don't expect
	 * the list to be long.
	 */
	void sortActives() {
		TRangeSrcDrPtrVec& vec = active_;
		size_t sz = vec.size();
		// Selection sort / removal outer loop
		for(size_t i = 0; i < sz;) {
			// Remove elements that we're done with
			if(vec[i]->done && !vec[i]->foundRange) {
				vec.erase(vec.begin() + i);
				if(sz == 0) break;
				else sz--;
				continue;
			}
			uint16_t minCost = vec[i]->minCost;
			size_t minOff = i;
			// Selection sort inner loop
			for(size_t j = i+1; j < sz; j++) {
				if(vec[j]->done && !vec[j]->foundRange) {
					// We'll get rid of this guy later
					continue;
				}
				if(vec[j]->minCost < minCost) {
					minCost = vec[j]->minCost;
					minOff = j;
				} else if(vec[j]->minCost == minCost) {
					// Possibly randomly pick the other
					if(rand_.nextU32() & 0x1000) {
						minOff = j;
					}
				}
			}
			// Do the swap, if necessary
			if(i != minOff) {
				assert_leq(minCost, vec[i]->minCost);
				TRangeSrcDrPtr tmp = vec[i];
				vec[i] = vec[minOff];
				vec[minOff] = tmp;
			}
			i++;
		}
		if(delayedRange_ == NULL) {
			assert_geq(this->minCost, this->minCostAdjustment_);
			assert_geq(vec[0]->minCost, this->minCost);
			this->minCost = vec[0]->minCost;
		}
		assert(sortedActives());
	}

#ifndef NDEBUG
	/**
	 * Check that the rss_ array is sorted by minCost; assert if it's
	 * not.
	 */
	bool sortedActives() const {
		// Selection sort outer loop
		const TRangeSrcDrPtrVec& vec = active_;
		const size_t sz = vec.size();
		for(size_t i = 0; i < sz; i++) {
			assert(!vec[i]->done || vec[i]->foundRange);
			for(size_t j = i+1; j < sz; j++) {
				assert(!vec[j]->done || vec[j]->foundRange);
				assert_leq(vec[i]->minCost, vec[j]->minCost);
			}
		}
		if(delayedRange_ == NULL && sz > 0) {
			// Only assert this if there's no delayed range; if there's
			// a delayed range, the minCost is its cost, not the 0th
			// element's cost
			assert_leq(vec[0]->minCost, this->minCost);
		}
		return true;
	}
#endif

	/// List of all the drivers
	TRangeSrcDrPtrVec rss_;
	/// List of all the as-yet-uneliminated drivers
	TRangeSrcDrPtrVec active_;
	/// Whether the list of drivers contains drivers for both mates 1 and 2
	bool paired_;
	/// If true, this driver will make an attempt to dish out ranges in
	/// a way that approaches the right distribution based on the
	/// number of hits on both strands.
	bool strandFix_;
	uint32_t randSeed_;
	/// The random seed from the Aligner, which we use to randomly break ties
	RandomSource rand_;
	Range *lastRange_;
	Range *delayedRange_;
	PatternSourcePerThread* patsrc_;
	bool verbose_;
	ASSERT_ONLY(std::set<int64_t> allTopsRc_);
};

#endif /* RANGE_SOURCE_H_ */
