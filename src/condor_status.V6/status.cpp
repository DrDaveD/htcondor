/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_config.h"
#include "condor_state.h"
#include "condor_api.h"
#include "status_types.h"
#include "totals.h"
#include "get_daemon_name.h"
#include "daemon.h"
#include "dc_collector.h"
#include "extArray.h"
#include "sig_install.h"
#include "string_list.h"
#include "condor_string.h"   // for strnewp()
#include "match_prefix.h"    // is_arg_colon_prefix
#include "print_wrapped_text.h"
#include "error_utils.h"
#include "condor_distribution.h"
#include "condor_version.h"
#include "natural_cmp.h"

#include <vector>
#include <sstream>
#include <iostream>

#define USE_LATE_PROJECTION 1
#define USE_QUERY_CALLBACKS 1

// if enabled, use natural_cmp for numerical sorting of hosts/slots
#define STRVCMP (naturalSort ? natural_cmp : strcmp)

#ifdef USE_QUERY_CALLBACKS

// the condor q strategy will be to ingest ads and print them into MyRowOfValues structures
// then insert those into a map which is indexed (and thus ordered) by job id.
// Once all of the jobs have arrived we linkup the JobRowOfData structures by job id and dag id.
// Finally, then we adjust column widths and formats based on the actual data and finally print.

// This structure hold the rendered fields from a single job ad, it will be inserted in to a map by job id
// and also linked up using the various pointers
//
class StatusRowOfData {
public:
	MyRowOfValues rov;
	unsigned int ordinal;  // id assigned to preserve the original order
	unsigned int flags;    // SROD_* flags used while processing
	ClassAd * ad;
	class StatusRowOfData * next_slot; // used at runtime to linkup slots for a machine.
	StatusRowOfData(unsigned int _ord)
		: ordinal(_ord), flags(0), ad(NULL)
		, next_slot(NULL)
	{}

	~StatusRowOfData() { if (ad) delete ad; ad = NULL; }

	bool isValid(int index) {
		if ( ! rov.is_valid(index)) return false;
		return rov.Column(index) != NULL;
	}

	template <class t>
	bool getNumber(int index, t & val) {
		val = 0;
		if ( ! rov.is_valid(index)) return false;
		classad::Value * pval = rov.Column(index);
		if ( ! pval) return false;
		return pval->IsNumber(val);
	}

	template <class s>
	bool getString(int index, s & val) {
		if ( ! rov.is_valid(index)) return false;
		classad::Value * pval = rov.Column(index);
		if ( ! pval) return false;
		return pval->IsStringValue(val);
	}
	bool getString(int index, char * buf, int cch) {
		if ( ! rov.is_valid(index)) return false;
		classad::Value * pval = rov.Column(index);
		if ( ! pval) return false;
		return pval->IsStringValue(buf, cch);
	}
};

struct STRVCMPStr {
   inline bool operator( )( const std::string &s1, const std::string &s2 ) const {
       return( natural_cmp( s1.c_str( ), s2.c_str( ) ) < 0 );
	}
};

typedef std::map<std::string, StatusRowOfData, STRVCMPStr> ROD_MAP_BY_KEY;

#define SROD_COOKED  0x0001   // set when the row data has been cooked (i.e. folded into)
#define SROD_SKIP    0x0002   // this row should be skipped entirely
#define SROD_FOLDED  0x0004   // data from this row has been folded into another row
#define SROD_PRINTED 0x0008   // Set during printing so we can know what has already been printed

#define SROD_PARTITIONABLE_SLOT 0x1000 // is a partitionable slot
#define SROD_BUSY_SLOT          0x2000 // is an busy slot (Claimed/Matched/Preempting)
#define SROD_UNAVAIL_SLOT       0x4000 // is an unavailable slot (Owner/Drained/Delete?)
#define SROD_EXHAUSTED_SLOT     0x8000 // is a partitionable slot with no cores remaining.

class ClassadSortSpecs {
public:
	ClassadSortSpecs() {}
	~ClassadSortSpecs() {
		for (size_t ii = 0; ii < key_exprs.size(); ++ii) {
			if (key_exprs[ii]) { delete key_exprs[ii]; }
			key_exprs[ii] = NULL;
		}
	}

	bool empty() { return key_args.empty(); }

	// returns true on success, false if the arg did not parse as an attribute or classad expression
	bool Add(const char * arg) {
		ExprTree* expr = NULL;
		if (ParseClassAdRvalExpr(arg, expr)) {
			return false;
		}
		key_exprs.push_back(expr);
		key_args.push_back(arg);
		return true;
	}
	// make sure that the primary key is the same as arg, inserting arg as the first key if needed
	bool ForcePrimaryKey(const char * arg) {
		if ( ! key_args.size() || key_args[0].empty() || MATCH != strcasecmp(key_args[0].c_str(), arg)) {
			ExprTree* expr = NULL;
			if (ParseClassAdRvalExpr(arg, expr)) {
				return false;
			}
			key_exprs.insert(key_exprs.begin(), expr);
			key_args.insert(key_args.begin(), arg);
		}
		return true;
	}

	void RenderKey(std::string & key, unsigned int ord, ClassAd * ad);
	void AddToProjection(classad::References & proj);
	// for debugging, dump the sort expressions
	void dump(std::string & out, const char * sep);

protected:
	std::vector<std::string> key_args;
	std::vector<classad::ExprTree*> key_exprs;
};

#else

using std::vector;
using std::string;
using std::stringstream;

struct SortSpec {
    string arg;
    string keyAttr;
    string keyExprAttr;
    ExprTree* expr;
    ExprTree* exprLT;
    ExprTree* exprEQ;

    SortSpec(): arg(), keyAttr(), keyExprAttr(), expr(NULL), exprLT(NULL), exprEQ(NULL) {}
    ~SortSpec() {
        if (NULL != expr) delete expr;
        if (NULL != exprLT) delete exprLT;
        if (NULL != exprEQ) delete exprEQ;
    }

    SortSpec(const SortSpec& src): expr(NULL), exprLT(NULL), exprEQ(NULL) { *this = src; }
    SortSpec& operator=(const SortSpec& src) {
        if (this == &src) return *this;

        arg = src.arg;
        keyAttr = src.keyAttr;
        keyExprAttr = src.keyExprAttr;
        if (NULL != expr) delete expr;
        expr = src.expr->Copy();
        if (NULL != exprLT) delete exprLT;
        exprLT = src.exprLT->Copy();
        if (NULL != exprEQ) delete exprEQ;
        exprEQ = src.exprEQ->Copy();

        return *this;
    }
};

#endif

// global variables
AttrListPrintMask pm;
printmask_headerfooter_t pmHeadFoot = STD_HEADFOOT;
List<const char> pm_head; // The list of headings for the mask entries
std::vector<GroupByKeyInfo> group_by_keys; // TJ 8.1.5 for future use, ignored for now.
bool explicit_format = false;
bool using_print_format = false; // hack for now so we can get standard totals when using -print-format
bool disable_user_print_files = false; // allow command line to defeat use of default user print files.
const char		*DEFAULT= "<default>";
DCCollector* pool = NULL;
//AdTypes		type 	= (AdTypes) -1;
int			sdo_mode = SDO_NotSet;
ppOption	ppStyle	= PP_NOTSET;
ppOption	ppTotalStyle = PP_NOTSET; // used when setting PP_CUSTOM to keep track of how to do totals.
int			wantOnlyTotals 	= 0;
int			summarySize = -1;
bool        expert = false;
bool		wide_display = false; // when true, don't truncate field data
bool		invalid_fields_empty = false; // when true, print "" instead of "[?]" for missing data
const char * mode_constraint = NULL; // constraint set by mode
int			diagnose = 0;
const char* diagnostics_ads_file = NULL; // filename to write diagnostics query ads to, from -diagnose:<filename>
char*		direct = NULL;
char*       statistics = NULL;
const char*	genericType = NULL;
CondorQuery *query;
char		buffer[1024];
char		*myName;
#ifdef USE_QUERY_CALLBACKS
ClassadSortSpecs sortSpecs;
#else
vector<SortSpec> sortSpecs;
#endif
bool            noSort = false; // set to true to disable sorting entirely
bool            naturalSort = true;
bool            javaMode = false;
bool			vmMode = false;
bool			absentMode = false;
bool			offlineMode = false;
bool			compactMode = false;
char 		*target = NULL;
const char * ads_file = NULL; // read classads from this file instead of querying them from the collector
ClassAd		*targetAd = NULL;

classad::References projList;
StringList dashAttributes; // Attributes specifically requested via the -attributes argument

// instantiate templates

// function declarations
void usage 		();
void firstPass  (int, char *[]);
void secondPass (int, char *[]);
#ifdef USE_QUERY_CALLBACKS
// prototype for CollectorList:query, CondorQuery::processAds,
// and CondorQ::fetchQueueFromHostAndProcess callbacks.
// callback should return false to take ownership of the ad
typedef bool (*FNPROCESS_ADS_CALLBACK)(void* pv, ClassAd * ad);
static bool read_classad_file(const char *filename, FNPROCESS_ADS_CALLBACK callback, void* pv, const char * constr);
//void prettyPrint(ROD_MAP_BY_KEY &, TrackTotals *);
ppOption prettyPrintHeadings (bool any_ads);
void prettyPrintAd(ppOption pps, ClassAd *ad);
int getDisplayWidth(bool * is_piped=NULL);
const char* digest_state_and_activity(char * sa, State st, Activity ac); //  in prettyPrint.cpp
#else
void prettyPrint(ClassAdList &, TrackTotals *);
int  lessThanFunc(AttrList*,AttrList*,void*);
int  customLessThanFunc(AttrList*,AttrList*,void*);
static bool read_classad_file(const char *filename, ClassAdList &classads, const char * constr);
#endif

extern "C" int SetSyscalls (int) {return 0;}
extern	int setPPstyle (ppOption, int, const char *);
extern  void setPPwidth ();
extern  void dumpPPMode(FILE* out);
extern const char * getPPStyleStr (ppOption pps);
#ifdef USE_LATE_PROJECTION
extern void prettyPrintInitMask(classad::References & proj);
extern AdTypes setMode(int sdo_mode, int arg_index, const char * arg);
#else
extern	void setType    (const char *, int, const char *);
extern	void setMode 	(Mode, int, const char *);
extern	void dumpPPMask (std::string & out, AttrListPrintMask & mask);
#endif
extern  int  forced_display_width;

#ifdef USE_QUERY_CALLBACKS

#if 0 // not currently used.
// callback for CollectorList::query to build a classad list.
static bool AddToClassAdList(void * pv, ClassAd* ad) {
	ClassAdList * plist = (ClassAdList*)pv;
	plist->Insert(ad);
	return false; // return false to indicate we took ownership of the ad.
}
#endif


void ClassadSortSpecs::AddToProjection(classad::References & proj)
{
	ClassAd ad;
	for (size_t ii = 0; ii < key_exprs.size(); ++ii) {
		classad::ExprTree * expr = key_exprs[ii];
		if (expr) { ad.GetExternalReferences(expr, proj, true); }
	}
}

void ClassadSortSpecs::dump(std::string & out, const char * sep)
{
	ClassAd ad;
	for (size_t ii = 0; ii < key_exprs.size(); ++ii) {
		classad::ExprTree * expr = key_exprs[ii];
		if (expr) { ExprTreeToString(expr, out); }
		out += sep;
	}
}

void ClassadSortSpecs::RenderKey(std::string & key, unsigned int ord, ClassAd * ad)
{
	for (size_t ii = 0; ii < key_exprs.size(); ++ii) {
		classad::Value val;
		classad::ExprTree * expr = key_exprs[ii];
		if (expr && ad->EvaluateExpr(expr, val)) {
			std::string fld;
			switch (val.GetType()) {
			case classad::Value::REAL_VALUE: {
				// render real by treating the bits of the double as integer bits. this works because
				// IEEE 754 floats compare the same when the bits are compared as they do
				// when compared as doubles because the exponent is the upper bits.
				union { long long lval; double dval; };
				val.IsRealValue(dval);
				formatstr(fld, "%lld", lval);
				}
				break;
			case classad::Value::INTEGER_VALUE:
			case classad::Value::BOOLEAN_VALUE: {
				long long lval;
				val.IsNumber(lval);
				formatstr(fld, "%lld", lval);
				}
				break;
			case classad::Value::STRING_VALUE:
				val.IsStringValue(fld);
				break;
			default: {
				classad::ClassAdUnParser unp;
				unp.Unparse(fld, val);
				}
				break;
			}
			key += fld;
		}
		key += "\n";
	}

	// append the ordinal as the key of last resort
	formatstr_cat(key, "%08X", ord);
}

#if 0
static void make_status_key(std::string & key, unsigned int ord, ClassAd* ad)
{
	std::string fld;
	if ( ! ad->LookupString(ATTR_MACHINE, key)) key = "";
	key += "\n";

	if ( ! ad->LookupString(ATTR_NAME, fld)) fld = "";
	key += fld;
	key += "\n";

	//if ( ! ad->LookupString(ATTR_OPSYS, fld)) fld = "";
	//if ( ! ad->LookupString(ATTR_ARCH, fld)) fld = "";

	// append the ordinal as the key of last resort
	formatstr_cat(key, "\n%08X", ord);
}
#endif

static State LookupStartdState(ClassAd* ad)
{
	char state[32];
	if (!ad->LookupString (ATTR_STATE, state, sizeof(state))) return no_state;
	return string_to_state (state);
}

// arguments passed to the process_ads_callback
struct _process_ads_info {
   ROD_MAP_BY_KEY * pmap;
   TrackTotals *  totals;
   unsigned int  ordinal;
   int           columns;
   FILE *        hfDiag; // write raw ads to this file for diagnostic purposes
   unsigned int  diag_flags;
};

extern int startdCompact_ixCol_Platform;  // Platform
extern int startdCompact_ixCol_ActCode;   // ST State+Activity code
int max_totals_subkey = -1;

static bool process_ads_callback(void * pv,  ClassAd* ad)
{
	bool done_with_ad = true;
	struct _process_ads_info * pi = (struct _process_ads_info *)pv;
	ROD_MAP_BY_KEY * pmap = pi->pmap;
	TrackTotals *    totals = pi->totals;

	std::string key;
	unsigned int ord = pi->ordinal++;
	sortSpecs.RenderKey(key, ord, ad);
	//make_status_key(key, ord, ad);

	// if diagnose flag is passed, unpack the key and ad and print them to the diagnostics file
	if (pi->hfDiag) {
		if (pi->diag_flags & 1) {
			fprintf(pi->hfDiag, "#Key:");
			size_t ib = 0;
			do {
				size_t ix = key.find_first_of("\n", ib);
				size_t cb = ix != std::string::npos ?  ix-ib: std::string::npos;
				fprintf(pi->hfDiag, " / %s", key.substr(ib, cb).c_str());
				ib = ix+1;
				if (ix == std::string::npos) break;
			} while (ib != std::string::npos);
			fputc('\n', pi->hfDiag);
		}

		if (pi->diag_flags & 2) {
			fPrintAd(pi->hfDiag, *ad);
			fputc('\n', pi->hfDiag);
		}

		return true; // done processing this ad
	}

	std::pair<ROD_MAP_BY_KEY::iterator,bool> pp = pmap->insert(std::pair<std::string, StatusRowOfData>(key, ord));
	if( ! pp.second ) {
		fprintf( stderr, "Error: Two results with the same key.\n" );
		done_with_ad = true;
	} else {

		// we can do normal totals now. but compact mode totals we have to do after checking the slot type
		if (totals && ! compactMode) { totals->update(ad); }

		if (pi->columns) {
			StatusRowOfData & srod = pp.first->second;

			srod.rov.SetMaxCols(pi->columns);
			pm.render(srod.rov, ad);

			bool fPslot = false;
			if (ad->LookupBool(ATTR_SLOT_PARTITIONABLE, fPslot) && fPslot) {
				srod.flags |= SROD_PARTITIONABLE_SLOT;
				double cpus = 0;
				if (ad->LookupFloat(ATTR_CPUS, cpus)) {
					if (cpus < 0.1) srod.flags |= SROD_EXHAUSTED_SLOT;
				}
			} // else
			{
				State state = LookupStartdState(ad);
				if (state == matched_state || state == claimed_state || state == preempting_state)
					srod.flags |= SROD_BUSY_SLOT;
				else if (state == drained_state || state == owner_state || state == shutdown_state || state == delete_state)
					srod.flags |= SROD_UNAVAIL_SLOT;
			}
			if (totals && compactMode) {
				char keybuf[64] = " ";
				const char * subtot_key = keybuf;
				switch(ppTotalStyle) {
					case PP_SUBMITTER_NORMAL: srod.getString(0, keybuf, sizeof(keybuf)); break;
					case PP_SCHEDD_NORMAL: subtot_key = NULL; break;
					case PP_STARTD_STATE: subtot_key = NULL; break; /* use activity as key */
					default: srod.getString(startdCompact_ixCol_Platform, keybuf, sizeof(keybuf)); break;
				}
				if (subtot_key) {
					int len = strlen(subtot_key);
					max_totals_subkey = MAX(max_totals_subkey, len);
				}
				totals->update(ad, TOTALS_OPTION_ROLLUP_PARTITIONABLE | TOTALS_OPTION_IGNORE_DYNAMIC, subtot_key);
				if ((srod.flags & (SROD_PARTITIONABLE_SLOT | SROD_EXHAUSTED_SLOT)) == SROD_PARTITIONABLE_SLOT) {
					totals->update(ad, 0, subtot_key);
				}
			}

			// for compact mode, we are about to throw about child state and activity attributes
			// so roll them up now
			if (compactMode && fPslot && startdCompact_ixCol_ActCode >= 0) {
				State consensus_state = no_state;
				Activity consensus_activity = no_act;

				const bool preempting_wins = true;

				char tmp[32];
				classad::Value lval, val;
				const classad::ExprList* plst = NULL;
				// roll up child states into a consensus child state
				if (ad->EvaluateAttr("Child" ATTR_STATE, lval) && lval.IsListValue(plst)) {
					for (classad::ExprList::const_iterator it = plst->begin(); it != plst->end(); ++it) {
						const classad::ExprTree * pexpr = *it;
						if (pexpr->Evaluate(val) && val.IsStringValue(tmp,sizeof(tmp))) {
							State st = string_to_state(tmp);
							if (st >= no_state && st < _state_threshold_) {
								if (consensus_state != st) {
									if (consensus_state == no_state) consensus_state = st;
									else {
										if (preempting_wins && st == preempting_state) {
											consensus_state = st; break;
										} else {
											consensus_state = _state_threshold_;
										}
									}
								}
							}
						}
					}
				}

				// roll up child activity into a consensus child state
				if (ad->EvaluateAttr("Child" ATTR_ACTIVITY, lval) && lval.IsListValue(plst)) {
					for (classad::ExprList::const_iterator it = plst->begin(); it != plst->end(); ++it) {
						const classad::ExprTree * pexpr = *it;
						if (pexpr->Evaluate(val) && val.IsStringValue(tmp,sizeof(tmp))) {
							Activity ac = string_to_activity(tmp);
							if (ac >= no_act && ac < _act_threshold_) {
								if (consensus_activity != ac) {
									if (consensus_activity == no_act) consensus_activity = ac;
									else {
										if (preempting_wins && ac == vacating_act) {
											consensus_activity = ac; break;
										} else {
											consensus_activity = _act_threshold_;
										}
									}
								}
							}
						}
					}
				}

				// roll concensus state into parent slot state.
				srod.getString(startdCompact_ixCol_ActCode, tmp, 4);
				digest_state_and_activity(tmp+4, consensus_state, consensus_activity);
				char bsc = tmp[4], bac = tmp[4+1];
				char asc = tmp[0], aac = tmp[1];
				if ((asc == 'U' && aac == 'i') && (srod.flags & SROD_EXHAUSTED_SLOT)) {
					// For exhausted partitionable slots that are Unclaimed/Idle, just use the concensus state
					asc = bsc; aac = bac;
				} else if (asc == 'D' && bsc == 'C') {
					// if partitionable slot is stated Drained and the children are claimed,
					// show the overall state as Claimed/Retiring
					asc = bsc;
				}
				if (preempting_wins) {
					if (bsc == 'P') asc = bsc;
					if (bac == 'v') aac = bac;
				}
				if (consensus_state != no_state && asc != bsc) asc = '*';
				if (consensus_activity != no_act && aac != bac) aac = '*';
				if (tmp[0] != asc || tmp[1] != aac) {
					tmp[0] = asc; tmp[1] = aac; tmp[2] = 0;
					srod.rov.Column(startdCompact_ixCol_ActCode)->SetStringValue(tmp);
				}

			}

			done_with_ad = true;
		} else {
			pp.first->second.ad = ad;
			done_with_ad = false;
		}
	}

	return done_with_ad; // return false to indicate we took ownership of the passed in ad.
}

// return true if the strings are non-empty and match up to the first \n
// or match exactly if there is no \n
bool same_primary_key(const std::string & aa, const std::string & bb)
{
	size_t cb = aa.size();
	if ( ! cb) return false;
	for (size_t ix = 0; ix < cb; ++ix) {
		char ch = aa[ix];
		if (bb[ix] != ch) return false;
		if (ch == '\n') return true;
	}
	return bb.size() == cb;
}

extern int startdCompact_ixCol_FreeCpus;  // Cpus
extern int startdCompact_ixCol_MaxSlotMem;// Max(ChildMemory)
extern int startdCompact_ixCol_FreeMem;   // Memory
extern int startdCompact_ixCol_Slots;     // NumDynamicSlots
extern int startdCompact_ixCol_JobStarts; // RecentJobStarts

// fold slot bb into aa assuming startdCompact format
void fold_slot_result(StatusRowOfData & aa, StatusRowOfData * pbb)
{
	if (aa.rov.empty()) return;

	// If the destination slot is not partitionable and hasn't already been cooked, some setup work is needed.
	if ( ! (aa.flags & SROD_PARTITIONABLE_SLOT) && ! (aa.flags & SROD_COOKED)) {

		// The MaxMem column will be undefined or error, set it equal to Memory
		if (startdCompact_ixCol_FreeMem >= 0 && startdCompact_ixCol_MaxSlotMem >= 0) {
			double amem;
			aa.getNumber(startdCompact_ixCol_FreeMem, amem);
			aa.rov.Column(startdCompact_ixCol_MaxSlotMem)->SetRealValue(amem);
			aa.rov.set_col_valid(startdCompact_ixCol_MaxSlotMem, true);
		}

		// The FreeMem and FreeCpus columns should be set to 0 if the slot is busy (or unavailable?)
		if (aa.flags & SROD_BUSY_SLOT) {
			if (startdCompact_ixCol_FreeCpus >= 0) aa.rov.Column(startdCompact_ixCol_FreeCpus)->SetIntegerValue(0);
			if (startdCompact_ixCol_FreeMem >= 0) aa.rov.Column(startdCompact_ixCol_FreeMem)->SetRealValue(0.0);
		}

		// The Slots column should be set to 1
		if (startdCompact_ixCol_Slots >= 0) {
			aa.rov.Column(startdCompact_ixCol_Slots)->SetIntegerValue(1);
			aa.rov.set_col_valid(startdCompact_ixCol_Slots, true);
		}

		aa.flags |= SROD_COOKED;
	}

	if ( ! pbb)
		return;

	StatusRowOfData & bb = *pbb;

	// If the source slot is partitionable, we fold differently than if it is static
	bool partitionable = (bb.flags & SROD_PARTITIONABLE_SLOT) != 0;

	// calculate the memory size of the largest slot
	double amem = 0.0;
	double bmem = 0.0;

	if (startdCompact_ixCol_MaxSlotMem >= 0) {
		aa.getNumber(startdCompact_ixCol_MaxSlotMem, amem);
		bb.getNumber(partitionable ? startdCompact_ixCol_MaxSlotMem : startdCompact_ixCol_FreeMem, bmem);
		double maxslotmem = MAX(amem,bmem);
		aa.rov.Column(startdCompact_ixCol_MaxSlotMem)->SetRealValue(maxslotmem);
	}

	// Add FreeMem and FreeCpus from bb into aa if slot bb is not busy
	if (partitionable || !(bb.flags & SROD_BUSY_SLOT)) {
		if (startdCompact_ixCol_FreeMem >= 0) {
			aa.getNumber(startdCompact_ixCol_FreeMem, amem);
			aa.rov.Column(startdCompact_ixCol_FreeMem)->SetRealValue(bmem + amem);
		}

		int acpus, bcpus;
		if (startdCompact_ixCol_FreeCpus >= 0) {
			aa.getNumber(startdCompact_ixCol_FreeCpus, acpus);
			bb.getNumber(startdCompact_ixCol_FreeCpus, bcpus);
			aa.rov.Column(startdCompact_ixCol_FreeCpus)->SetIntegerValue(acpus + bcpus);
		}
	}

	// Increment the aa Slots column
	int aslots, bslots = 1;
	if (startdCompact_ixCol_Slots >= 0) {
		aa.getNumber(startdCompact_ixCol_Slots, aslots);
		if (partitionable) bb.getNumber(startdCompact_ixCol_Slots, bslots);
		aa.rov.Column(startdCompact_ixCol_Slots)->SetIntegerValue(aslots + bslots);
	}

	// Sum the number of job starts
	double astarts, bstarts;
	if (startdCompact_ixCol_JobStarts) {
		aa.getNumber(startdCompact_ixCol_JobStarts, astarts);
		bb.getNumber(startdCompact_ixCol_JobStarts, bstarts);
		aa.rov.Column(startdCompact_ixCol_JobStarts)->SetRealValue(astarts + bstarts);
	}

	// merge the state/activity (for static slots, partitionable merge happens elsewhere)
	if (startdCompact_ixCol_ActCode >= 0) {
		char ast[4] = {0,0,0,0}, bst[4] = {0,0,0,0};
		aa.getString(startdCompact_ixCol_ActCode, ast, sizeof(ast));
		bb.getString(startdCompact_ixCol_ActCode, bst, sizeof(bst));
		char asc = ast[0], bsc = bst[0], aac = ast[1], bac = bst[1];
		if (asc != bsc) asc = '*';
		if (aac != bac) aac = '*';
		if (ast[0] != asc || ast[1] != aac) {
			ast[0] = asc; ast[1] = aac;
			aa.rov.Column(startdCompact_ixCol_ActCode)->SetStringValue(ast);
		}
	}
}

void reduce_slot_results(ROD_MAP_BY_KEY & rmap)
{
	if (rmap.empty())
		return;

	ROD_MAP_BY_KEY::iterator it, itMachine = rmap.begin();
	it = itMachine;
	for (++it; it != rmap.end(); ++it) {
		if (same_primary_key(it->first, itMachine->first)) {
			fold_slot_result(itMachine->second, &it->second);
			it->second.flags |= SROD_FOLDED;
		} else {
			fold_slot_result(itMachine->second, NULL);
			itMachine = it;
		}
	}
}

#endif // USE_QUERY_CALLBACKS

int
main (int argc, char *argv[])
{
#if !defined(WIN32)
	install_sig_handler(SIGPIPE, (SIG_HANDLER)SIG_IGN );
#endif

	// initialize to read from config file
	myDistro->Init( argc, argv );
	myName = argv[0];
	config();
	dprintf_config_tool_on_error(0);

	// The arguments take two passes to process --- the first pass
	// figures out the mode, after which we can instantiate the required
	// query object.  We add implied constraints from the command line in
	// the second pass.
	firstPass (argc, argv);
	
	// if the mode has not been set, it is SDO_Startd, we set it here
	// but with default priority, so that any mode set in the first pass
	// takes precedence.
	// the actual adType to be queried is returned, note that this will
	// _not_ be STARTD_AD if another ad type was set in pass 1
	AdTypes adType = setMode (SDO_Startd, 0, DEFAULT);
	ASSERT(sdo_mode != SDO_NotSet);

	/*
	if (compactMode && (adType != STARTD_AD)) {
		fprintf(stderr, "Error: -compact option conflicts with type of ClassAd being queried.\n");
		exit(1);
	}
	*/

	// instantiate query object
	if (!(query = new CondorQuery (adType))) {
		dprintf_WriteOnErrorBuffer(stderr, true);
		fprintf (stderr, "Error:  Out of memory\n");
		exit (1);
	}
	// if a first-pass setMode set a mode_constraint, apply it now to the query object
	if (mode_constraint && ! explicit_format) {
		query->addANDConstraint(mode_constraint);
	}


		// if there was a generic type specified
	if (genericType) {
		// tell the query object what the type we're querying is
		if (diagnose) { printf ("Setting generic ad type to %s\n", genericType); }
		query->setGenericQueryType(genericType);
	}

	// set the constraints implied by the mode
	if (sdo_mode == SDO_Startd_Avail && ! compactMode) {
		// -avail shows unclaimed slots
		sprintf (buffer, "%s == \"%s\" && Cpus > 0", ATTR_STATE, state_to_string(unclaimed_state));
		if (diagnose) { printf ("Adding OR constraint [%s]\n", buffer); }
		query->addORConstraint (buffer);
	}
	else if (sdo_mode == SDO_Startd_Run && ! compactMode) {
		// -run shows claimed slots
		sprintf (buffer, "%s == \"%s\"", ATTR_STATE, state_to_string(claimed_state));
		if (diagnose) { printf ("Adding OR constraint [%s]\n", buffer); }
		query->addORConstraint (buffer);
	}
	else if (sdo_mode == SDO_Startd_Cod) {
		// -run shows claimed slots
		sprintf (buffer, ATTR_NUM_COD_CLAIMS " > 0");
		if (diagnose) { printf ("Adding OR constraint [%s]\n", buffer); }
		query->addORConstraint (buffer);
	}

	if(javaMode) {
		sprintf( buffer, "%s == TRUE", ATTR_HAS_JAVA );
		if (diagnose) {
			printf ("Adding constraint [%s]\n", buffer);
		}
		query->addANDConstraint (buffer);
		
		projList.insert(ATTR_HAS_JAVA);
		projList.insert(ATTR_JAVA_MFLOPS);
		projList.insert(ATTR_JAVA_VENDOR);
		projList.insert(ATTR_JAVA_VERSION);
	}

	if(offlineMode) {
		query->addANDConstraint( "size( OfflineUniverses ) != 0" );

		projList.insert( "OfflineUniverses" );

		//
		// Since we can't add a regex to a projection, explicitly list all
		// the attributes we know about.
		//

		projList.insert( "HasVM" );
		projList.insert( "VMOfflineReason" );
		projList.insert( "VMOfflineTime" );
	}

	if(absentMode) {
	    sprintf( buffer, "%s == TRUE", ATTR_ABSENT );
	    if (diagnose) {
	        printf( "Adding constraint %s\n", buffer );
	    }
	    query->addANDConstraint( buffer );
	    
	    projList.insert( ATTR_ABSENT );
	    projList.insert( ATTR_LAST_HEARD_FROM );
	    projList.insert( ATTR_CLASSAD_LIFETIME );
	}

	if(vmMode) {
		sprintf( buffer, "%s == TRUE", ATTR_HAS_VM);
		if (diagnose) {
			printf ("Adding constraint [%s]\n", buffer);
		}
		query->addANDConstraint (buffer);

		projList.insert(ATTR_VM_TYPE);
		projList.insert(ATTR_VM_MEMORY);
		projList.insert(ATTR_VM_NETWORKING);
		projList.insert(ATTR_VM_NETWORKING_TYPES);
		projList.insert(ATTR_VM_HARDWARE_VT);
		projList.insert(ATTR_VM_AVAIL_NUM);
		projList.insert(ATTR_VM_ALL_GUEST_MACS);
		projList.insert(ATTR_VM_ALL_GUEST_IPS);
		projList.insert(ATTR_VM_GUEST_MAC);
		projList.insert(ATTR_VM_GUEST_IP);
	}

	if (compactMode && ! (vmMode || javaMode)) {
		if (sdo_mode == SDO_Startd_Avail) {
			// State==Unclaimed picks up partitionable and unclaimed static, Cpus > 0 picks up only partitionable that have free memory
			sprintf(buffer, "State == \"%s\" && Cpus > 0 && Memory > 0", state_to_string(unclaimed_state));
		} else if (sdo_mode == SDO_Startd_Run) {
			// State==Claimed picks up static slots, NumDynamicSlots picks up partitionable slots that are partly claimed.
			sprintf(buffer, "(State == \"%s\" && DynamicSlot =!= true) || (NumDynamicSlots isnt undefined && NumDynamicSlots > 0)", state_to_string(claimed_state));
		} else {
			sprintf(buffer, "PartitionableSlot =?= true || DynamicSlot =!= true");
		}
		if (diagnose) {
			printf ("Adding constraint [%s]\n", buffer);
		}
		query->addANDConstraint (buffer);
		projList.insert(ATTR_ARCH);
		projList.insert(ATTR_OPSYS_AND_VER);
		projList.insert(ATTR_OPSYS_NAME);
		projList.insert(ATTR_SLOT_DYNAMIC);
		projList.insert(ATTR_SLOT_PARTITIONABLE);
		projList.insert(ATTR_STATE);
		projList.insert(ATTR_ACTIVITY);
		//projList.insert(ATTR_MACHINE_RESOURCES);
		//projList.insert(ATTR_WITHIN_RESOURCE_LIMITS); // this will force all partitionable resource values to be fetched.
		projList.insert("ChildState"); // this is needed to do the summary rollup
		projList.insert("ChildActivity"); // this is needed to do the summary rollup
		//pmHeadFoot = (printmask_headerfooter_t)(pmHeadFoot | HF_NOSUMMARY);
	}

	// second pass:  add regular parameters and constraints
	if (diagnose) {
		printf ("----------\n");
	}

	secondPass (argc, argv);

	if (sortSpecs.empty() && ! noSort) {
		// set a default sort of Machine/Name
		sortSpecs.Add(ATTR_MACHINE);
		sortSpecs.Add(ATTR_NAME);
	}
	if (compactMode) {
		// compact mode reqires machine to be the primary sort key.
		sortSpecs.ForcePrimaryKey(ATTR_MACHINE);
	}
	sortSpecs.AddToProjection(projList);

	// initialize the totals object
	if (ppStyle == PP_CUSTOM && using_print_format) {
		if (pmHeadFoot & HF_NOSUMMARY) ppTotalStyle = PP_CUSTOM;
	} else {
		ppTotalStyle = ppStyle;
	}
	TrackTotals	totals(ppTotalStyle);

	// in order to totals, the projection MUST have certain attributes
	if (wantOnlyTotals || ((ppTotalStyle != PP_CUSTOM) && ! projList.empty())) {
		switch (ppTotalStyle) {
			case PP_STARTD_SERVER:
				projList.insert(ATTR_MEMORY);
				projList.insert(ATTR_DISK);
				// fall through
			case PP_STARTD_RUN:
				projList.insert(ATTR_LOAD_AVG);
				projList.insert(ATTR_MIPS);
				projList.insert(ATTR_KFLOPS);
				// fall through
			case PP_STARTD_NORMAL:
			case PP_STARTD_COD:
				if (ppTotalStyle == PP_STARTD_COD) {
					projList.insert(ATTR_CLAIM_STATE);
					projList.insert(ATTR_COD_CLAIMS);
				}
				projList.insert(ATTR_STATE); // Norm, state, server
				projList.insert(ATTR_ARCH);  // for key
				projList.insert(ATTR_OPSYS); // for key
				break;

			case PP_STARTD_STATE:
				projList.insert(ATTR_STATE);
				projList.insert(ATTR_ACTIVITY); // for key
				break;

			case PP_SUBMITTER_NORMAL:
				projList.insert(ATTR_NAME); // for key
				projList.insert(ATTR_RUNNING_JOBS);
				projList.insert(ATTR_IDLE_JOBS);
				projList.insert(ATTR_HELD_JOBS);
				break;

			case PP_SCHEDD_NORMAL: // no key
				projList.insert(ATTR_TOTAL_RUNNING_JOBS);
				projList.insert(ATTR_TOTAL_IDLE_JOBS);
				projList.insert(ATTR_TOTAL_HELD_JOBS);
				break;

			case PP_CKPT_SRVR_NORMAL:
				projList.insert(ATTR_DISK);
				break;

			default:
				break;
		}
	}

	// fetch the query
	QueryResult q;

	if( ppStyle == PP_VERBOSE || ppStyle == PP_XML || ppStyle == PP_JSON ) {
		// Remove everything from the projection list if we're displaying
		// the "long form" of the ads.
		projList.clear();

		// but if -attributes was supplied, show only those attributes
		if ( ! dashAttributes.isEmpty()) {
			const char * s;
			dashAttributes.rewind();
			while ((s = dashAttributes.next())) {
				projList.insert(s);
			}
		}
		pmHeadFoot = HF_BARE;
	}

#ifdef USE_LATE_PROJECTION
	// Setup the pretty printer for the given mode.
	if (ppStyle != PP_VERBOSE && ppStyle != PP_XML && ppStyle != PP_JSON && ppStyle != PP_CUSTOM) {
		prettyPrintInitMask(projList);
	}
#endif

	// if diagnose was requested, just print the query ad
	if (diagnose) {

		FILE* fout = stderr;

		fprintf(fout, "diagnose: ");
		for (int ii = 0; ii < argc; ++ii) { fprintf(fout, "%s ", argv[ii]); }
		fprintf(fout, "\n----------\n");

		// print diagnostic information about inferred internal state
		dumpPPMode(fout);
		fprintf(fout, "Totals: %s\n", getPPStyleStr(ppTotalStyle));
		fprintf(fout, "Opts: HF=%x\n", pmHeadFoot);

		std::string style_text;
		style_text.reserve(8000);
		style_text = "";

		sortSpecs.dump(style_text, " ] [ ");
		fprintf(fout, "Sort: [ %s<ord> ]\n", style_text.c_str());

		style_text = "";
	#ifdef USE_LATE_PROJECTION
		const CustomFormatFnTable * getCondorStatusPrintFormats();
		List<const char> * pheadings = NULL;
		if ( ! pm.has_headings()) {
			if (pm_head.Length() > 0) pheadings = &pm_head;
		}
		pm.dump(style_text, getCondorStatusPrintFormats(), pheadings);
	#else
		dumpPPMask (style_text, pm);
	#endif
		fprintf(fout, "\nPrintMask:\n%s\n", style_text.c_str());

		ClassAd queryAd;
		q = query->getQueryAd (queryAd);
		fPrintAd (fout, queryAd);

		// print projection
		if (projList.empty()) {
			fprintf(fout, "Projection: <NULL>\n");
		} else {
			fprintf(fout, "Projection:\n");
			classad::References::const_iterator it;
			for (it = projList.begin(); it != projList.end(); ++it) {
				fprintf(fout, "  %s\n",  it->c_str());
			}
		}

		fprintf (fout, "\n\n");
		fprintf (stdout, "Result of making query ad was:  %d\n", q);
		if ( ! diagnostics_ads_file) exit (1);
	}

	if ( ! projList.empty()) {
		#if 0 // for debugging
		std::string attrs;
		attrs.reserve(projList.size()*24);
		for (classad::References::const_iterator it = projList.begin(); it != projList.end(); ++it) {
			if ( ! attrs.empty()) attrs += " ";
			attrs += *it;
		}
		fprintf(stdout, "Projection is [%s]\n", attrs.c_str());
		#endif
		query->setDesiredAttrs(projList);
	}

	// Address (host:port) is taken from requested pool, if given.
	char* addr = (NULL != pool) ? pool->addr() : NULL;
	Daemon* requested_daemon = pool;

	// If we're in "direct" mode, then we attempt to locate the daemon
	// associated with the requested subsystem (here encoded by value of mode)
	// In this case the host:port of pool (if given) denotes which
	// pool is being consulted
	if( direct ) {
		Daemon *d = NULL;
		switch (adType) {
		case MASTER_AD: d = new Daemon( DT_MASTER, direct, addr ); break;
		case STARTD_AD: d = new Daemon( DT_STARTD, direct, addr ); break;
		case QUILL_AD:  d = new Daemon( DT_QUILL, direct, addr ); break;
		case SCHEDD_AD:
		case SUBMITTOR_AD: d = new Daemon( DT_SCHEDD, direct, addr ); break;
		case NEGOTIATOR_AD:
		case ACCOUNTING_AD: d = new Daemon( DT_NEGOTIATOR, direct, addr ); break;
		default: // These have to go to the collector, there is no 'direct'
			break;
		}

		// Here is where we actually override 'addr', if we can obtain
		// address of the requested daemon/subsys.  If it can't be
		// located, then fail with error msg.
		// 'd' will be null (unset) if mode is one of above that must go to
		// collector (MODE_ANY_NORMAL, MODE_COLLECTOR_NORMAL, etc)
		if (NULL != d) {
			if( d->locate() ) {
				addr = d->addr();
				requested_daemon = d;
			} else {
				const char* id = d->idStr();
				if (NULL == id) id = d->name();
				dprintf_WriteOnErrorBuffer(stderr, true);
				if (NULL == id) id = "daemon";
				fprintf(stderr, "Error: Failed to locate %s\n", id);
				fprintf(stderr, "%s\n", d->error());
				exit( 1 );
			}
		}
	}

#ifdef USE_QUERY_CALLBACKS
	CondorError errstack;
	ROD_MAP_BY_KEY admap;
	struct _process_ads_info ai = {
		&admap,
		(pmHeadFoot&HF_NOSUMMARY) ? NULL : &totals,
		1, // key of last resort, this counts up as members are added.
		pm.ColCount(), // if 0, the ad is stored in the map, otherwise a row of data is stored and the add freed.
		NULL, 0, // diagnostic file, flags
	};

	bool close_hfdiag = false;
	if (diagnose) {
		ai.diag_flags = 1 | 2;
		if (diagnostics_ads_file && diagnostics_ads_file[0] != '-') {
			ai.hfDiag = safe_fopen_wrapper_follow(diagnostics_ads_file, "w");
			if ( ! ai.hfDiag) {
				fprintf(stderr, "\nERROR: Failed to open file -diag output file (%s)\n", strerror(errno));
			}
			close_hfdiag = true;
		} else {
			ai.hfDiag = stdout;
			if (diagnostics_ads_file && diagnostics_ads_file[0] == '-' && diagnostics_ads_file[1] == '2') ai.hfDiag = stderr;
			close_hfdiag = false;
		}
		if ( ! ai.hfDiag) { exit(2); }
	}

	if (NULL != ads_file) {
		MyString req; // query requirements
		q = query->getRequirements(req);
		const char * constraint = req.empty() ? NULL : req.c_str();
		if (read_classad_file(ads_file, process_ads_callback, &ai, constraint)) {
			q = Q_OK;
		}
	} else if (NULL != addr) {
			// this case executes if pool was provided, or if in "direct" mode with
			// subsystem that corresponds to a daemon (above).
			// Here 'addr' represents either the host:port of requested pool, or
			// alternatively the host:port of daemon associated with requested subsystem (direct mode)
		q = query->processAds (process_ads_callback, &ai, addr, &errstack);
	} else {
			// otherwise obtain list of collectors and submit query that way
		CollectorList * collectors = CollectorList::create();
		q = collectors->query (*query, process_ads_callback, &ai, &errstack);
		delete collectors;
	}
	if (diagnose) {
		if (close_hfdiag) {
			fclose(ai.hfDiag);
			ai.hfDiag = NULL;
		}
		exit(1);
	}

	if (compactMode) {
		switch (adType) {
		case STARTD_AD: reduce_slot_results(admap); break;
		default: break;
		}
	}
#else
	ClassAdList result;
	CondorError errstack;
	if (NULL != ads_file) {
		MyString req; // query requirements
		q = query->getRequirements(req);
		const char * constraint = req.empty() ? NULL : req.c_str();
		if (read_classad_file(ads_file, result, constraint)) {
			q = Q_OK;
		}
	} else if (NULL != addr) {
			// this case executes if pool was provided, or if in "direct" mode with
			// subsystem that corresponds to a daemon (above).
			// Here 'addr' represents either the host:port of requested pool, or
			// alternatively the host:port of daemon associated with requested subsystem (direct mode)
		q = query->fetchAds (result, addr, &errstack);
	} else {
			// otherwise obtain list of collectors and submit query that way
		CollectorList * collectors = CollectorList::create();
		q = collectors->query (*query, result, &errstack);
		delete collectors;
	}
#endif

	// if any error was encountered during the query, report it and exit 
	if (Q_OK != q) {

		dprintf_WriteOnErrorBuffer(stderr, true);
			// we can always provide these messages:
		fprintf( stderr, "Error: %s\n", getStrQueryResult(q) );
		fprintf( stderr, "%s\n", errstack.getFullText(true).c_str() );

		if ((NULL != requested_daemon) && ((Q_NO_COLLECTOR_HOST == q) ||
			(requested_daemon->type() == DT_COLLECTOR)))
		{
				// Specific long message if connection to collector failed.
			const char* fullhost = requested_daemon->fullHostname();
			if (NULL == fullhost) fullhost = "<unknown_host>";
			const char* daddr = requested_daemon->addr();
			if (NULL == daddr) daddr = "<unknown>";
			char info[1000];
			sprintf(info, "%s (%s)", fullhost, daddr);
			printNoCollectorContact( stderr, info, !expert );
		} else if ((NULL != requested_daemon) && (Q_COMMUNICATION_ERROR == q)) {
				// more helpful message for failure to connect to some daemon/subsys
			const char* id = requested_daemon->idStr();
			if (NULL == id) id = requested_daemon->name();
			if (NULL == id) id = "daemon";
			const char* daddr = requested_daemon->addr();
			if (NULL == daddr) daddr = "<unknown>";
			fprintf(stderr, "Error: Failed to contact %s at %s\n", id, daddr);
		}

		// fail
		exit (1);
	}

#ifdef USE_QUERY_CALLBACKS
	bool any_ads = ! admap.empty();
	ppOption pps = prettyPrintHeadings (any_ads);

	bool is_piped = false;
	int display_width = getDisplayWidth(&is_piped);
	std::string line; line.reserve(is_piped ? 1024 : display_width);

	// for XML output, print the xml header even if there are no ads.
	if (PP_XML == pps) {
		std::string line;
		AddClassAdXMLFileHeader(line);
		fputs(line.c_str(), stdout); // xml string already ends in a newline.
	}
	if (PP_JSON == pps) {
		printf("[\n");
	}

	for (ROD_MAP_BY_KEY::iterator it = admap.begin(); it != admap.end(); ++it) {
		if (it->second.flags & (SROD_FOLDED | SROD_SKIP))
			continue;
		if (ai.columns) {
			line.clear();
			pm.display(line, it->second.rov);
			fputs(line.c_str(), stdout);
		} else {
			prettyPrintAd(pps, it->second.ad);
		}
		it->second.flags |= SROD_PRINTED; // for debugging, keep track of what we already printed.
	}
	
	// for XML output, print the xml footer even if there are no ads.
	if (PP_XML == pps) {
		AddClassAdXMLFileFooter(line);
		fputs(line.c_str(), stdout);
		// PRAGMA_REMIND("tj: XML output used to have an extra trailing newline, do we need to preserve that?")
	}
	if (PP_JSON == pps) {
		printf("]\n");
	}

	// if totals are required, display totals
	if (any_ads && !(pmHeadFoot&HF_NOSUMMARY) && totals.haveTotals()) {
		fputc('\n', stdout);
		bool auto_width = (ppTotalStyle == PP_SUBMITTER_NORMAL);
		int totals_key_width = (wide_display || auto_width) ? -1 : MAX(20, max_totals_subkey);
		totals.displayTotals(stdout, totals_key_width);
	}
#else
	//ClassAdList & result = adlist;
	if (noSort) {
		// do nothing 
	} else if (sortSpecs.empty()) {
        // default classad sorting
		result.Sort((SortFunctionType)lessThanFunc);
	} else {
        // User requested custom sorting expressions:
        // insert attributes related to custom sorting
        result.Open();
        while (ClassAd* ad = result.Next()) {
            for (vector<SortSpec>::iterator ss(sortSpecs.begin());  ss != sortSpecs.end();  ++ss) {
                ss->expr->SetParentScope(ad);
                classad::Value v;
                ss->expr->Evaluate(v);
                stringstream vs;
                // This will properly render all supported value types,
                // including undefined and error, although current semantic
                // pre-filters classads where sort expressions are undef/err:
                vs << ((v.IsStringValue())?"\"":"") << v << ((v.IsStringValue())?"\"":"");
                ad->AssignExpr(ss->keyAttr.c_str(), vs.str().c_str());
                // Save the full expr in case user wants to examine on output:
                ad->AssignExpr(ss->keyExprAttr.c_str(), ss->arg.c_str());
            }
        }
        
        result.Open();
		result.Sort((SortFunctionType)customLessThanFunc);
	}

	
	// output result
	prettyPrint (result, &totals);
#endif

    delete query;

	return 0;
}

const CustomFormatFnTable * getCondorStatusPrintFormats();


int set_status_print_mask_from_stream (
	const char * streamid,
	bool is_filename,
	const char ** pconstraint)
{
	std::string where_expr;
	std::string messages;
	StringList attrs;
	printmask_aggregation_t aggregation;

	SimpleInputStream * pstream = NULL;
	*pconstraint = NULL;

	FILE *file = NULL;
	if (MATCH == strcmp("-", streamid)) {
		pstream = new SimpleFileInputStream(stdin, false);
	} else if (is_filename) {
		file = safe_fopen_wrapper_follow(streamid, "r");
		if (file == NULL) {
			fprintf(stderr, "Can't open select file: %s\n", streamid);
			return -1;
		}
		pstream = new SimpleFileInputStream(file, true);
	} else {
		pstream = new StringLiteralInputStream(streamid);
	}
	ASSERT(pstream);

	int err = SetAttrListPrintMaskFromStream(
					*pstream,
					*getCondorStatusPrintFormats(),
					pm,
					pmHeadFoot,
					aggregation,
					group_by_keys,
					where_expr,
					attrs,
					messages);
	delete pstream; pstream = NULL;
	if ( ! err) {
		if (aggregation != PR_NO_AGGREGATION) {
			fprintf(stderr, "print-format aggregation not supported\n");
			return -1;
		}

		if ( ! where_expr.empty()) {
			*pconstraint = pm.store(where_expr.c_str());
			//if ( ! validate_constraint(*pconstraint)) {
			//	formatstr_cat(messages, "WHERE expression is not valid: %s\n", *pconstraint);
			//}
		}
		// convert projection list into the format that condor status likes. because programmers.
		for (const char * attr = attrs.first(); attr; attr = attrs.next()) { projList.insert(attr); }
	}
	if ( ! messages.empty()) { fprintf(stderr, "%s", messages.c_str()); }
	return err;
}

#ifdef USE_QUERY_CALLBACKS
static bool read_classad_file(const char *filename, FNPROCESS_ADS_CALLBACK callback, void* pv, const char * constr)
#else
static bool read_classad_file(const char *filename, ClassAdList &classads, const char * constr)
#endif
{
	bool success = false;

	FILE* file = NULL;
	bool close_file = false;
	if (MATCH == strcmp(filename,"-")) {
		file = stdin;
		close_file = false;
	} else {
		file = safe_fopen_wrapper_follow(filename, "r");
		close_file = true;
	}
	if (file == NULL) {
		fprintf(stderr, "Can't open file of job ads: %s\n", filename);
		return false;
	} else {
		CondorClassAdFileParseHelper parse_helper("\n");

		for (;;) {
			ClassAd* classad = new ClassAd();

			int error;
			bool is_eof;
			int cAttrs = classad->InsertFromFile(file, is_eof, error, &parse_helper);

			bool include_classad = cAttrs > 0 && error >= 0;
			if (include_classad && constr) {
				classad::Value val;
				if (classad->EvaluateExpr(constr,val)) {
					if ( ! val.IsBooleanValueEquiv(include_classad)) {
						include_classad = false;
					}
				}
			}
#ifdef USE_QUERY_CALLBACKS
			if ( ! include_classad || callback(pv, classad)) {
				// delete the classad if we didn't pass it to the callback, or if
				// the callback didn't take ownership of it.
				delete classad;
			}
#else
			if (include_classad) {
				classads.Insert(classad);
			} else {
				delete classad;
			}
#endif

			if (is_eof) {
				success = true;
				break;
			}
			if (error < 0) {
				success = false;
				break;
			}
		}

		if (close_file) fclose(file);
		file = NULL;
	}
	return success;
}


void
usage ()
{
	fprintf (stderr,"Usage: %s [help-opt] [query-opt] [custom-opts] [display-opts] [name ...]\n", myName);

	fprintf (stderr,"    where [help-opt] is one of\n"
		"\t-help\t\t\tPrint this screen and exit\n"
		"\t-version\t\tPrint HTCondor version and exit\n"
		"\t-diagnose\t\tPrint out query ad without performing query\n"
		);

	fprintf (stderr,"\n    and [query-opt] is one of\n"
		"\t-absent\t\t\tPrint information about absent resources\n"
		"\t-avail\t\t\tPrint information about available resources\n"
		"\t-ckptsrvr\t\tDisplay checkpoint server attributes\n"
		"\t-claimed\t\tPrint information about claimed resources\n"
		"\t-cod\t\t\tDisplay Computing On Demand (COD) jobs\n"
		"\t-collector\t\tDisplay collector daemon attributes\n"
		"\t-debug\t\t\tDisplay debugging info to console\n"
		"\t-defrag\t\t\tDisplay status of defrag daemon\n"
		"\t-direct <host>\t\tGet attributes directly from the given daemon\n"
		"\t-java\t\t\tDisplay Java-capable hosts\n"
		"\t-vm\t\t\tDisplay VM-capable hosts\n"
		"\t-license\t\tDisplay attributes of licenses\n"
		"\t-master\t\t\tDisplay daemon master attributes\n"
		"\t-pool <name>\t\tGet information from collector <name>\n"
		"\t-ads <file>\t\tGet information from <file>\n"
        "\t-grid\t\t\tDisplay grid resources\n"
		"\t-run\t\t\tSame as -claimed [deprecated]\n"
#ifdef HAVE_EXT_POSTGRESQL
		"\t-quill\t\t\tDisplay attributes of quills\n"
#endif /* HAVE_EXT_POSTGRESQL */
		"\t-schedd\t\t\tDisplay attributes of schedds\n"
		"\t-server\t\t\tDisplay important attributes of resources\n"
		"\t-startd\t\t\tDisplay resource attributes\n"
		"\t-generic\t\tDisplay attributes of 'generic' ads\n"
		"\t-subsystem <type>\tDisplay classads of the given type\n"
		"\t-negotiator\t\tDisplay negotiator attributes\n"
		"\t-storage\t\tDisplay network storage resources\n"
		"\t-any\t\t\tDisplay any resources\n"
		"\t-state\t\t\tDisplay state of resources\n"
		"\t-submitters\t\tDisplay information about request submitters\n"
//		"\t-world\t\t\tDisplay all pools reporting to UW collector\n"
		);

	fprintf (stderr, "\n    and [custom-opts ...] are one or more of\n"
		"\t-constraint <const>\tAdd constraint on classads\n"
		"\t-compact\t\t\tShow compact form, rolling up slots into a single line\n"
		"\t-statistics <set>:<n>\tDisplay statistics for <set> at level <n>\n"
		"\t\t\t\tsee STATISTICS_TO_PUBLISH for valid <set> and level values\n"
		"\t\t\t\tuse with -direct queries to STARTD and SCHEDD daemons\n"
		"\t-target <file>\t\tUse target classad with -format or -af evaluation\n"
		"\n    and [display-opts] are one or more of\n"
		"\t-long\t\t\tDisplay entire classads\n"
		"\t-sort <expr>\t\tSort entries by expressions. 'no' disables sorting\n"
		"\t-natural[:off]\t\tUse natural sort order in default output (default=on)\n"
		"\t-total\t\t\tDisplay totals only\n"
//		"\t-verbose\t\tSame as -long\n"
		"\t-expert\t\t\tDisplay shorter error messages\n"
		"\t-wide[:<width>]\t\tDon't truncate data to fit in 80 columns.\n"
		"\t\t\t\tTruncates to console width or <width> argument if specified.\n"
		"\t-xml\t\t\tDisplay entire classads, but in XML\n"
		"\t-json\t\t\tDisplay entire classads, but in JSON\n"
		"\t-attributes X,Y,...\tAttributes to show in -xml or -long \n"
		"\t-format <fmt> <attr>\tDisplay <attr> values with formatting\n"
		"\t-autoformat[:lhVr,tng] <attr> [<attr2> [...]]\n"
		"\t-af[:lhVr,tng] <attr> [attr2 [...]]\n"
		"\t    Print attr(s) with automatic formatting\n"
		"\t    the [lhVr,tng] options modify the formatting\n"
		//"\t        j   Display Job id\n"
		"\t        l   attribute labels\n"
		"\t        h   attribute column headings\n"
		"\t        V   %%V formatting (string values are quoted)\n"
		"\t        r   %%r formatting (raw/unparsed values)\n"
		"\t        t   tab before each value (default is space)\n"
		"\t        g   newline between ClassAds, no space before values\n"
		"\t        ,   comma after each value\n"
		"\t        n   newline after each value\n"
		"\t    use -af:h to get tabular values with headings\n"
		"\t    use -af:lrng to get -long equivalant format\n"
		"\t-print-format <file>\tUse <file> to set display attributes and formatting\n"
		"\t\t\t(experimental, see htcondor-wiki for more information)\n"
		);
}

void
firstPass (int argc, char *argv[])
{
	int had_pool_error = 0;
	int had_direct_error = 0;
	int had_statistics_error = 0;
	//bool explicit_mode = false;
	const char * pcolon = NULL;

	// Process arguments:  there are dependencies between them
	// o -l/v and -serv are mutually exclusive
	// o -sub, -avail and -run are mutually exclusive
	// o -pool and -entity may be used at most once
	// o since -c can be processed only after the query has been instantiated,
	//   constraints are added on the second pass
	for (int i = 1; i < argc; i++) {
		if (is_dash_arg_prefix (argv[i], "avail", 2)) {
			setMode (SDO_Startd_Avail, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "pool", 1)) {
			if( pool ) {
				delete pool;
				had_pool_error = 1;
			}
			i++;
			if( ! argv[i] ) {
				fprintf( stderr, "%s: -pool requires a hostname as an argument.\n",
						 myName );
				if (!expert) {
					printf("\n");
					print_wrapped_text("Extra Info: The hostname should be the central "
									   "manager of the Condor pool you wish to work with.",
									   stderr);
					printf("\n");
				}
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			pool = new DCCollector( argv[i] );
			if( !pool->addr() ) {
				dprintf_WriteOnErrorBuffer(stderr, true);
				fprintf( stderr, "Error: %s\n", pool->error() );
				if (!expert) {
					printf("\n");
					print_wrapped_text("Extra Info: You specified a hostname for a pool "
									   "(the -pool argument). That should be the Internet "
									   "host name for the central manager of the pool, "
									   "but it does not seem to "
									   "be a valid hostname. (The DNS lookup failed.)",
									   stderr);
				}
				exit( 1 );
			}
		} else
		if (is_dash_arg_prefix (argv[i], "ads", 2)) {
			if( !argv[i+1] ) {
				fprintf( stderr, "%s: -ads requires a filename argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			i += 1;
			ads_file = argv[i];
		} else
		if (is_dash_arg_prefix (argv[i], "format", 1)) {
			setPPstyle (PP_CUSTOM, i, argv[i]);
			if( !argv[i+1] || !argv[i+2] ) {
				fprintf( stderr, "%s: -format requires two other arguments\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			i += 2;
			pmHeadFoot = HF_BARE;
			explicit_format = true;
		} else
		if (*argv[i] == '-' &&
			(is_arg_colon_prefix(argv[i]+1, "autoformat", &pcolon, 5) || 
			 is_arg_colon_prefix(argv[i]+1, "af", &pcolon, 2)) ) {
				// make sure we have at least one more argument
			if ( !argv[i+1] || *(argv[i+1]) == '-') {
				fprintf( stderr, "Error: Argument %s requires "
						 "at last one attribute parameter\n", argv[i] );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			pmHeadFoot = HF_NOSUMMARY;
			explicit_format = true;
			setPPstyle (PP_CUSTOM, i, argv[i]);
			while (argv[i+1] && *(argv[i+1]) != '-') {
				++i;
			}
			// if autoformat list ends in a '-' without any characters after it, just eat the arg and keep going.
			if (i+1 < argc && '-' == (argv[i+1])[0] && 0 == (argv[i+1])[1]) {
				++i;
			}
		} else
		if (is_dash_arg_colon_prefix(argv[i], "print-format", &pcolon, 2)) {
			if ( (i+1 >= argc)  || (*(argv[i+1]) == '-' && (argv[i+1])[1] != 0)) {
				fprintf( stderr, "Error: Argument -print-format requires a filename argument\n");
				exit( 1 );
			}
			explicit_format = true;
			++i; // eat the next argument.
			// we can't fully parse the print format argument until the second pass, so we are done for now.
		} else
		if (is_dash_arg_colon_prefix (argv[i], "wide", &pcolon, 3)) {
			wide_display = true; // when true, don't truncate field data
			if (pcolon) {
				forced_display_width = atoi(++pcolon);
				if (forced_display_width <= 100) wide_display = false;
				setPPwidth();
			}
			//invalid_fields_empty = true;
		} else
		if (is_dash_arg_colon_prefix (argv[i], "natural", &pcolon, 3)) {
			naturalSort = true;
			if (pcolon) {
				if (MATCH == strcmp(++pcolon,"off")) {
					naturalSort = false;
				}
			}
		} else
		if (is_dash_arg_prefix (argv[i], "target", 4)) {
			if( !argv[i+1] ) {
				fprintf( stderr, "%s: -target requires one additional argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			i += 1;
			target = argv[i];
			FILE *targetFile = safe_fopen_wrapper_follow(target, "r");
			int iseof, iserror, empty;
			targetAd = new ClassAd(targetFile, "\n\n", iseof, iserror, empty);
			fclose(targetFile);
		} else
		if (is_dash_arg_prefix (argv[i], "constraint", 3)) {
			// can add constraints on second pass only
			i++;
			if( ! argv[i] ) {
				fprintf( stderr, "%s: -constraint requires another argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
		} else
		if (is_dash_arg_prefix (argv[i], "direct", 3)) {
			if( direct ) {
				free( direct );
				had_direct_error = 1;
			}
			i++;
			if( ! argv[i] ) {
				fprintf( stderr, "%s: -direct requires another argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			direct = strdup( argv[i] );
		} else
		if (is_dash_arg_colon_prefix (argv[i], "diagnose", &pcolon, 3)) {
			diagnose = 1;
			if (pcolon) diagnostics_ads_file = ++pcolon;
		} else
		if (is_dash_arg_prefix (argv[i], "debug", 2)) {
			// dprintf to console
			dprintf_set_tool_debug("TOOL", 0);
		} else
		if (is_dash_arg_prefix (argv[i], "defrag", 3)) {
			setMode (SDO_Defrag, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "help", 1)) {
			usage ();
			exit (0);
		} else
		if (is_dash_arg_prefix (argv[i], "long", 1) /* || matchPrefix (argv[i],"-verbose", 3)*/) {
			//PRAGMA_REMIND("tj: remove -verbose as a synonym for -long")
			setPPstyle (PP_VERBOSE, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i],"xml", 1)){
			setPPstyle (PP_XML, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i],"json", 2)){
			setPPstyle (PP_JSON, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i],"attributes", 2)){
			if( !argv[i+1] ) {
				fprintf( stderr, "%s: -attributes requires one additional argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			i++;
		} else	
		if (is_dash_arg_prefix(argv[i], "claimed", 2) || is_dash_arg_prefix (argv[i], "run", 1)) {
			setMode (SDO_Startd_Run, i, argv[i]);
		} else
		if( is_dash_arg_prefix (argv[i], "cod", 3) ) {
			setMode (SDO_Startd_Cod, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "java", 1)) {
			/*explicit_mode =*/ javaMode = true;
		} else
		if (is_dash_arg_prefix (argv[i], "absent", 2)) {
			/*explicit_mode =*/ absentMode = true;
		} else
		if (is_dash_arg_prefix (argv[i], "offline", 2)) {
			/*explicit_mode =*/ offlineMode = true;
		} else
		if (is_dash_arg_prefix (argv[i], "vm", 2)) {
			/*explicit_mode =*/ vmMode = true;
		} else
		if (is_dash_arg_prefix (argv[i], "slots", 2)) {
			setMode (SDO_Startd, i, argv[i]);
			compactMode = false;
		} else
		if (is_dash_arg_prefix (argv[i], "compact", 3)) {
			compactMode = true;
		} else
		if (is_dash_arg_prefix (argv[i], "nocompact", 5)) {
			compactMode = false;
		} else
		if (is_dash_arg_prefix (argv[i], "server", 2)) {
			//PRAGMA_REMIND("TJ: change to sdo_mode")
			setPPstyle (PP_STARTD_SERVER, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "state", 4)) {
			//PRAGMA_REMIND("TJ: change to sdo_mode")
			setPPstyle (PP_STARTD_STATE, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "statistics", 5)) {
			if( statistics ) {
				free( statistics );
				had_statistics_error = 1;
			}
			i++;
			if( ! argv[i] ) {
				fprintf( stderr, "%s: -statistics requires another argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			statistics = strdup( argv[i] );
		} else
		if (is_dash_arg_prefix (argv[i], "startd", 4)) {
			setMode (SDO_Startd,i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "schedd", 2)) {
			setMode (SDO_Schedd, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "grid", 1)) {
			setMode (SDO_Grid, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "subsystem", 4)) {
			i++;
			if( !argv[i] || *argv[i] == '-') {
				fprintf( stderr, "%s: -subsystem requires another argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}
			static const struct { const char * tag; int sm; } asub[] = {
				{"schedd", SDO_Schedd},
				{"submitters", SDO_Submitters},
				{"startd", SDO_Startd},
				{"quill", SDO_Quill},
				{"defrag", SDO_Defrag},
				{"grid", SDO_Grid},
				{"accounting", SDO_Accounting},
				{"negotiator", SDO_Negotiator},
				{"master", SDO_Master},
				{"collector", SDO_Collector},
				{"generic", SDO_Generic},
				{"had", SDO_HAD},
			};
			int sm = SDO_NotSet;
			for (int ii = 0; ii < (int)COUNTOF(asub); ++ii) {
				if (is_arg_prefix(argv[i], asub[ii].tag, -1)) {
					sm = asub[ii].sm;
					break;
				}
			}
			if (sm != SDO_NotSet) {
				setMode (sm, i, argv[i]);
			} else {
				genericType = argv[i];
				setMode (SDO_Other, i, argv[i]);
			}
		} else
#ifdef HAVE_EXT_POSTGRESQL
		if (is_dash_arg_prefix (argv[i], "quill", 1)) {
			setMode (SDO_Quill, i, argv[i]);
		} else
#endif /* HAVE_EXT_POSTGRESQL */
		if (is_dash_arg_prefix (argv[i], "license", 2)) {
			setMode (SDO_License, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "storage", 3)) {
			setMode (SDO_Storage, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "negotiator", 1)) {
			setMode (SDO_Negotiator, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "generic", 2)) {
			setMode (SDO_Generic, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "any", 2)) {
			setMode (SDO_Any, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "sort", 2)) {
			i++;
			if( ! argv[i] ) {
				fprintf( stderr, "%s: -sort requires another argument\n",
						 myName );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}

			if (MATCH == strcasecmp(argv[i], "false") ||
				MATCH == strcasecmp(argv[i], "0") ||
				MATCH == strcasecmp(argv[i], "no") ||
				MATCH == strcasecmp(argv[i], "none"))
			{
				noSort = true;
				continue;
			}

#ifdef USE_QUERY_CALLBACKS
			if ( ! sortSpecs.Add(argv[i])) {
				fprintf(stderr, "Error:  Parse error of: %s\n", argv[i]);
				exit(1);
			}
#else
            int jsort = sortSpecs.size();
            SortSpec ss;
			ExprTree* sortExpr = NULL;
			if (ParseClassAdRvalExpr(argv[i], sortExpr)) {
				fprintf(stderr, "Error:  Parse error of: %s\n", argv[i]);
				exit(1);
			}
            ss.expr = sortExpr;

            ss.arg = argv[i];
            formatstr(ss.keyAttr, "CondorStatusSortKey%d", jsort);
            formatstr(ss.keyExprAttr, "CondorStatusSortKeyExpr%d", jsort);

			string exprString;
			formatstr(exprString, "MY.%s < TARGET.%s", ss.keyAttr.c_str(), ss.keyAttr.c_str());
			if (ParseClassAdRvalExpr(exprString.c_str(), sortExpr)) {
                fprintf(stderr, "Error:  Parse error of: %s\n", exprString.c_str());
                exit(1);
			}
			ss.exprLT = sortExpr;

			formatstr(exprString, "MY.%s == TARGET.%s", ss.keyAttr.c_str(), ss.keyAttr.c_str());
			if (ParseClassAdRvalExpr(exprString.c_str(), sortExpr)) {
                fprintf(stderr, "Error:  Parse error of: %s\n", exprString.c_str());
                exit(1);
			}
			ss.exprEQ = sortExpr;

            sortSpecs.push_back(ss);
				// the silent constraint TARGET.%s =!= UNDEFINED is added
				// as a customAND constraint on the second pass
#endif
		} else
		if (is_dash_arg_prefix (argv[i], "submitters", 4)) {
			setMode (SDO_Submitters, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "master", 1)) {
			setMode (SDO_Master, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "collector", 3)) {
			setMode (SDO_Collector, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "world", 1)) {
			setMode (SDO_Collector, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "ckptsrvr", 2)) {
			setMode (SDO_CkptSvr, i, argv[i]);
		} else
		if (is_dash_arg_prefix (argv[i], "total", 1)) {
			wantOnlyTotals = 1;
			pmHeadFoot = (printmask_headerfooter_t)(HF_NOTITLE | HF_NOHEADER);
			explicit_format = true;
		} else
		if (is_dash_arg_prefix(argv[i], "expert", 1)) {
			expert = true;
		} else
		if (is_dash_arg_prefix(argv[i], "version", 3)) {
			printf( "%s\n%s\n", CondorVersion(), CondorPlatform() );
			exit(0);
		} else
		if (*argv[i] == '-') {
			fprintf (stderr, "Error:  Unknown option %s\n", argv[i]);
			usage ();
			exit (1);
		}
	}
	if( had_pool_error ) {
		fprintf( stderr,
				 "Warning:  Multiple -pool arguments given, using \"%s\"\n",
				 pool->name() );
	}
	if( had_direct_error ) {
		fprintf( stderr,
				 "Warning:  Multiple -direct arguments given, using \"%s\"\n",
				 direct );
	}
	if( had_statistics_error ) {
		fprintf( stderr,
				 "Warning:  Multiple -statistics arguments given, using \"%s\"\n",
				 statistics );
	}
}


void
secondPass (int argc, char *argv[])
{
	const char * pcolon = NULL;
	char *daemonname;
	for (int i = 1; i < argc; i++) {
		// omit parameters which qualify switches
		if( is_dash_arg_prefix(argv[i],"pool", 1) || is_dash_arg_prefix(argv[i],"direct", 3) ) {
			i++;
			continue;
		}
		if( is_dash_arg_prefix(argv[i],"subsystem", 4) ) {
			i++;
			continue;
		}
		if (is_dash_arg_prefix (argv[i], "format", 1)) {
			pm.registerFormat (argv[i+1], argv[i+2]);

			StringList attributes;
			ClassAd ad;
			if(!ad.GetExprReferences(argv[i+2],NULL,&attributes)){
				fprintf( stderr, "Error:  Parse error of: %s\n", argv[i+2]);
				exit(1);
			}

			for (const char * attr = attributes.first(); attr; attr = attributes.next()) {
				projList.insert(attr);
			}

			if (diagnose) {
				printf ("Arg %d --- register format [%s] for [%s]\n",
						i, argv[i+1], argv[i+2]);
			}
			i += 2;
			continue;
		}
		if (*argv[i] == '-' &&
			(is_arg_colon_prefix(argv[i]+1, "autoformat", &pcolon, 5) || 
			 is_arg_colon_prefix(argv[i]+1, "af", &pcolon, 2)) ) {
				// make sure we have at least one more argument
			if ( !argv[i+1] || *(argv[i+1]) == '-') {
				fprintf( stderr, "Error: Argument %s requires "
						 "at last one attribute parameter\n", argv[i] );
				fprintf( stderr, "Use \"%s -help\" for details\n", myName );
				exit( 1 );
			}

			bool flabel = false;
			bool fCapV  = false;
			bool fRaw = false;
			bool fheadings = false;
			const char * prowpre = NULL;
			const char * pcolpre = " ";
			const char * pcolsux = NULL;
			if (pcolon) {
				++pcolon;
				while (*pcolon) {
					switch (*pcolon)
					{
						case ',': pcolsux = ","; break;
						case 'n': pcolsux = "\n"; break;
						case 'g': pcolpre = NULL; prowpre = "\n"; break;
						case 't': pcolpre = "\t"; break;
						case 'l': flabel = true; break;
						case 'V': fCapV = true; break;
						case 'r': case 'o': fRaw = true; break;
						case 'h': fheadings = true; break;
					}
					++pcolon;
				}
			}
			pm.SetAutoSep(prowpre, pcolpre, pcolsux, "\n");

			while (argv[i+1] && *(argv[i+1]) != '-') {
				++i;
				ClassAd ad;
				StringList attributes;
				if(!ad.GetExprReferences(argv[i],NULL,&attributes)){
					fprintf( stderr, "Error:  Parse error of: %s\n", argv[i]);
					exit(1);
				}

				//PRAGMA_REMIND("fix to use more set-based GetExprReferences")
				for (const char * attr = attributes.first(); attr; attr = attributes.next()) {
					projList.insert(attr);
				}

				MyString lbl = "";
				int wid = 0;
				int opts = FormatOptionNoTruncate;
				if (fheadings || pm_head.Length() > 0) { 
					const char * hd = fheadings ? argv[i] : "(expr)";
					wid = 0 - (int)strlen(hd); 
					opts = FormatOptionAutoWidth | FormatOptionNoTruncate; 
					pm_head.Append(hd);
				}
				else if (flabel) { lbl.formatstr("%s = ", argv[i]); wid = 0; opts = 0; }
				lbl += fRaw ? "%r" : (fCapV ? "%V" : "%v");
				if (diagnose) {
					printf ("Arg %d --- register format [%s] width=%d, opt=0x%x for [%s]\n",
							i, lbl.Value(), wid, opts,  argv[i]);
				}
				pm.registerFormat(lbl.Value(), wid, opts, argv[i]);
			}
			// if autoformat list ends in a '-' without any characters after it, just eat the arg and keep going.
			if (i+1 < argc && '-' == (argv[i+1])[0] && 0 == (argv[i+1])[1]) {
				++i;
			}
			continue;
		}
		if (is_dash_arg_colon_prefix(argv[i], "print-format", &pcolon, 2)) {
			if ( (i+1 >= argc)  || (*(argv[i+1]) == '-' && (argv[i+1])[1] != 0)) {
				fprintf( stderr, "Error: Argument -print-format requires a filename argument\n");
				exit( 1 );
			}
			// hack allow -pr ! to disable use of user-default print format files.
			if (MATCH == strcmp(argv[i+1], "!")) {
				++i;
				disable_user_print_files = true;
				continue;
			}
			ppTotalStyle = ppStyle;
			setPPstyle (PP_CUSTOM, i, argv[i]);
			setPPwidth();
			++i; // skip to the next argument.
			if (set_status_print_mask_from_stream(argv[i], true, &mode_constraint) < 0) {
				fprintf(stderr, "Error: invalid select file %s\n", argv[i]);
				exit (1);
			}
			if (mode_constraint) {
				query->addANDConstraint(mode_constraint);
			}
			using_print_format = true; // so we can hack totals.
			continue;
		}
		if (is_dash_arg_prefix (argv[i], "target", 4)) {
			i++;
			continue;
		}
		if (is_dash_arg_prefix(argv[i], "ads", 2)) {
			++i;
			continue;
		}
		if( is_dash_arg_prefix(argv[i], "sort", 2) ) {
			i++;
			if ( ! noSort) {
				sprintf( buffer, "%s =!= UNDEFINED", argv[i] );
				query->addANDConstraint( buffer );
			}
			continue;
		}
		
		if (is_dash_arg_prefix (argv[i], "statistics", 5)) {
			i += 2;
			sprintf(buffer,"STATISTICS_TO_PUBLISH = \"%s\"", statistics);
			if (diagnose) {
				printf ("[%s]\n", buffer);
			}
			query->addExtraAttribute(buffer);
			continue;
		}

		if (is_dash_arg_prefix (argv[i], "attributes", 2) ) {
			// parse attributes to be selected and split them along ","
			StringList more_attrs(argv[i+1],",");
			for (const char * s = more_attrs.first(); s; s = more_attrs.next()) {
				projList.insert(s);
				dashAttributes.append(s);
			}

			i++;
			continue;
		}
		


		// figure out what the other parameters should do
		if (*argv[i] != '-') {
			// display extra information for diagnosis
			if (diagnose) {
				printf ("Arg %d (%s) --- adding constraint", i, argv[i]);
			}

			const char * name = argv[i];
			daemonname = get_daemon_name(name);
			if( ! daemonname || ! daemonname[0]) {
				if ( (sdo_mode == SDO_Submitters) && strchr(argv[i],'@') ) {
					// For a submittor query, it is possible that the
					// hostname is really a UID_DOMAIN.  And there is
					// no requirement that UID_DOMAIN actually have
					// an inverse lookup in DNS...  so if get_daemon_name()
					// fails with a fully qualified submittor lookup, just
					// use what we are given and do not flag an error.
				} else {
					//PRAGMA_REMIND("TJ: change this to do the query rather than reporting an error?")
					dprintf_WriteOnErrorBuffer(stderr, true);
					fprintf( stderr, "%s: unknown host %s\n",
								 argv[0], get_host_part(argv[i]) );
					exit(1);
				}
			} else {
				name = daemonname;
			}

			if (sdo_mode == SDO_Startd_Run) {
				sprintf (buffer, ATTR_REMOTE_USER " == \"%s\"", argv[i]);
				if (diagnose) { printf ("[%s]\n", buffer); }
				query->addORConstraint (buffer);
			} else {
				sprintf(buffer, ATTR_NAME "==\"%s\" || " ATTR_MACHINE "==\"%s\"", name, name);
				if (diagnose) { printf ("[%s]\n", buffer); }
				query->addORConstraint (buffer);
			}
			delete [] daemonname;
			daemonname = NULL;
		} else
		if (is_dash_arg_prefix (argv[i], "constraint", 3)) {
			if (diagnose) { printf ("[%s]\n", argv[i+1]); }
			query->addANDConstraint (argv[i+1]);
			i++;
		}
	}
}


#ifdef USE_QUERY_CALLBACKS
#else

int
lessThanFunc(AttrList *ad1, AttrList *ad2, void *)
{
	MyString  buf1;
	MyString  buf2;
	int       val;

	if( !ad1->LookupString(ATTR_OPSYS, buf1) ||
		!ad2->LookupString(ATTR_OPSYS, buf2) ) {
		buf1 = "";
		buf2 = "";
	}
	val = strcmp( buf1.Value(), buf2.Value() );
	if( val ) {
		return (val < 0);
	}

	if( !ad1->LookupString(ATTR_ARCH, buf1) ||
		!ad2->LookupString(ATTR_ARCH, buf2) ) {
		buf1 = "";
		buf2 = "";
	}
	val = strcmp( buf1.Value(), buf2.Value() );
	if( val ) {
		return (val < 0);
	}

	if( !ad1->LookupString(ATTR_MACHINE, buf1) ||
		!ad2->LookupString(ATTR_MACHINE, buf2) ) {
		buf1 = "";
		buf2 = "";
	}
	val = STRVCMP( buf1.Value(), buf2.Value() );
	if( val ) {
		return (val < 0);
	}

	if (!ad1->LookupString(ATTR_NAME, buf1) ||
		!ad2->LookupString(ATTR_NAME, buf2))
		return 0;
	return ( STRVCMP( buf1.Value(), buf2.Value() ) < 0 );
}


int
customLessThanFunc( AttrList *ad1, AttrList *ad2, void *)
{
	classad::Value lt_result;
	bool val;

	for (unsigned i = 0;  i < sortSpecs.size();  ++i) {
		if (EvalExprTree(sortSpecs[i].exprLT, ad1, ad2, lt_result)
			&& lt_result.IsBooleanValue(val) ) {
			if( val ) {
				return 1;
			} else {
				if (EvalExprTree( sortSpecs[i].exprEQ, ad1,
					ad2, lt_result ) &&
					( !lt_result.IsBooleanValue(val) || !val )){
					return 0;
				}
			}
		} else {
			return 0;
		}
	}
	return 0;
}
#endif
