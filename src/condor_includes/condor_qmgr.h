/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
  *
  * Condor Software Copyright Notice
  * Copyright (C) 1990-2006, Condor Team, Computer Sciences Department,
  * University of Wisconsin-Madison, WI.
  *
  * This source code is covered by the Condor Public License, which can
  * be found in the accompanying LICENSE.TXT file, or online at
  * www.condorproject.org.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * AND THE UNIVERSITY OF WISCONSIN-MADISON "AS IS" AND ANY EXPRESS OR
  * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  * WARRANTIES OF MERCHANTABILITY, OF SATISFACTORY QUALITY, AND FITNESS
  * FOR A PARTICULAR PURPOSE OR USE ARE DISCLAIMED. THE COPYRIGHT
  * HOLDERS AND CONTRIBUTORS AND THE UNIVERSITY OF WISCONSIN-MADISON
  * MAKE NO MAKE NO REPRESENTATION THAT THE SOFTWARE, MODIFICATIONS,
  * ENHANCEMENTS OR DERIVATIVE WORKS THEREOF, WILL NOT INFRINGE ANY
  * PATENT, COPYRIGHT, TRADEMARK, TRADE SECRET OR OTHER PROPRIETARY
  * RIGHT.
  *
  ****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/
#if !defined(_LIBQMGR_H)
#define _LIBQMGR_H

#include "condor_common.h"
#include "condor_io.h"
#include "proc.h"
#include "../condor_c++_util/CondorError.h"
class ClassAd;
class ClassAdList;


typedef struct {
	bool dummy;
} Qmgr_connection;

typedef int (*scan_func)(ClassAd *ad);

typedef unsigned char SetAttributeFlags_t;
const SetAttributeFlags_t NONDURABLE = 1;

#define SHADOW_QMGMT_TIMEOUT 300

// QmgmtPeer* getQmgmtConnectionInfo();
// bool setQmgmtConnectionInfo(QmgmtPeer *peer);
void unsetQmgmtConnection();


int InitializeConnection(const char *, const char *);
int InitializeReadOnlyConnection(const char * );

/** Initiate connection to schedd job queue and begin transaction.
	@param qmgr_location can be the name or sinful string of a schedd or
	       NULL to connect to the local schedd
    @param timeout specifies the maximum time (in seconds) to wait for TCP
	       connection establishment
    @param read_only can be set to true to skip the potentially slow
	       authenticate step for connections which don't modify the queue
	@return opaque Qmgr_connection structure
*/		 
Qmgr_connection *ConnectQ(char *qmgr_location, int timeout=0, 
				bool read_only=false, CondorError* errstack=NULL );

/** Close the connection to the schedd job queue, and optionally commit
	the transaction.
	@param qmgr pointer to Qmgr_connection object returned by ConnectQ
	@param commit_transactions set to true to commit the transaction, 
	and false to abort the transaction.
	@return true if commit was successful; false if transaction was aborted
*/
bool DisconnectQ(Qmgr_connection *qmgr, bool commit_transactions=true);

/** Start a new job cluster.  This cluster becomes the
	active cluster, and jobs may only be submitted to this cluster.
	@return -1 on failure; the new cluster id on success
*/
int NewCluster(void);
/** Signal the start of a new job description (a new job process).
	@param cluster_id cluster id of the active job cluster (from NewCluster())
	@return -1 on failure; the new proc id on success
*/
int NewProc( int cluster_id);

const int DESTROYPROC_SUCCESS_DELAY = 1; // DestoryProc succeeded. The job is still enqueued, but that's okay
const int DESTROYPROC_SUCCESS = 0; // DestroyProc succeeded
const int DESTROYPROC_ERROR = -1; // DestroyProc failed in a non-specific way
const int DESTROYPROC_EACCES = -2; // DestroyProc failed: wrong user or other access problem
const int DESTROYPROC_ENOENT = -3; // DestroyProc failed: cluster.proc doesn't exist.
/** Remove job with cluster_id and proc_id from the queue.  This is a
	low-level mechanism.  Normally, to remove jobs from the queue, set the
	job status to REMOVED and send a KILL_FRGN_JOB command to the schedd.
	@return: 0 or greater is success.  Specific results are DESTROYPROC_SUCCESS(0) = job removed and DESTROYPROC_SUCCESS_DELAY(1) = job not _yet_ removed, but "done".  Negative numbers indicate failure.  Specific failures are DESTROYPROC_ERROR(-1) = Unknown/non-specific error, DESTROYPROC_EACCESS(-2) = Owner failed, and DESTROYPROC_ENOENT(-3) = Job doesn't exist.
*/
int DestroyProc(int cluster_id, int proc_id);
/** Remove a cluster of jobs from the queue.
*/
int DestroyCluster(int cluster_id, const char *reason = NULL);
/** For all jobs in the queue for which constraint evaluates to true, set
	attr = value.  The value should be a valid ClassAd value (strings
	should be surrounded by quotes).
	@return -1 on failure; 0 on success
*/
int SetAttributeByConstraint(const char *constraint, const char *attr,
							 const char *value);
/** For all jobs in the queue for which constraint evaluates to true, set
	attr = value.  The value should be a valid ClassAd value (strings
	should be surrounded by quotes).
	@return -1 on failure; 0 on success
*/
int SetAttributeIntByConstraint(const char *constraint, const char *attr,
								int value);
/** For all jobs in the queue for which constraint evaluates to true, set
	attr = value.  The value should be a valid ClassAd value (strings
	should be surrounded by quotes).
	@return -1 on failure; 0 on success
*/
int SetAttributeFloatByConstraint(const char *constraing, const char *attr,
								  float value);
/** For all jobs in the queue for which constraint evaluates to true, set
	attr = value.  The value should be a valid ClassAd value (strings
	should be surrounded by quotes).
	@return -1 on failure; 0 on success
*/
int SetAttributeStringByConstraint(const char *constraint, const char *attr,
								   const char *value);
/** Set attr = value for job with specified cluster and proc.  The value
	should be a valid ClassAd value (strings should be surrounded by
	quotes)
	@return -1 on failure; 0 on success
*/
int SetAttribute(int cluster, int proc, const char *attr, const char *value, SetAttributeFlags_t flags=0 );
/** Set attr = value for job with specified cluster and proc.  The value
	should be a valid ClassAd value (strings should be surrounded by
	quotes)
	@return -1 on failure; 0 on success
*/
int SetAttributeInt(int cluster, int proc, const char *attr, int value, SetAttributeFlags_t flags = 0 );
/** Set attr = value for job with specified cluster and proc.  The value
	should be a valid ClassAd value (strings should be surrounded by
	quotes)
	@return -1 on failure; 0 on success
*/
int SetAttributeFloat(int cluster, int proc, const char *attr, float value);
/** Set attr = value for job with specified cluster and proc.  The value
	should be a valid ClassAd value (strings should be surrounded by
	quotes)
	@return -1 on failure; 0 on success
*/
int SetAttributeString(int cluster, int proc, const char *attr,
					   const char *value);

/** Set LastJobLeaseRenewalReceived = <xact start time> and
    JobLeaseDurationReceived = dur for the specified cluster/proc.
	@return -1 on failure; 0 on success
*/
int SetTimerAttribute(int cluster, int proc, const char *attr_name, int dur);

/** Set the password to the MyProxy server for specified cluster/proc. The
	value should be a null-terminated string.
	@return -1 on failure; 0 on success
*/
int SetMyProxyPassword (int cluster, int proc, const char * pwd);


int CloseConnection();
bool InTransaction();
void BeginTransaction();
void CommitTransaction(SetAttributeFlags_t flags=0);
void AbortTransaction();
void AbortTransactionAndRecomputeClusters();


void AbortTransaction();

/** Get value of attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int GetAttributeFloat(int cluster, int proc, const char *attr, float *value);
/** Get value of attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int GetAttributeInt(int cluster, int proc, const char *attr, int *value);
/** Get value of attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int GetAttributeBool(int cluster, int proc, const char *attr, int *value);
/** Get value of attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int GetAttributeString(int cluster, int proc, const char *attr, char *value);
/** Get value of string attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success. Allocates new copy of the string.
*/
int GetAttributeStringNew( int cluster_id, int proc_id, const char *attr_name, 
					   char **val );
/** Get value of attr for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int GetAttributeExpr(int cluster, int proc, const char *attr, char *value);
/** Delete specified attribute for job with specified cluster and proc.
	@return -1 on failure; 0 on success
*/
int DeleteAttribute(int cluster, int proc, const char *attr);

/** Efficiently get the entire job ClassAd.
	The caller MUST call FreeJobAd when the ad is no longer in use. 
	@param cluster_id Cluster number of ad to fetch
	@param proc_id Process number of ad to fetch
	@param expStartdAttrs Expand $$(xxx) style macros inside the ClassAd
		with attributes from the matching Startd ad.  
	@return NULL on failure; the job ClassAd on success
*/
ClassAd *GetJobAd(int cluster_id, int proc_id, bool expStardAttrs = false);

/** Efficiently get the first job ClassAd which matches the constraint.
	@return NULL on failure; the job ClassAd on success
*/
ClassAd *GetJobByConstraint(const char *constraint);
/** Efficiently get the all jobs ClassAd which matches the constraint.
*/
void GetAllJobsByConstraint(const char *constraint, const char *proj,ClassAdList &list);
/** Iterate over all jobs in the queue.
	The caller MUST call FreeJobAd when the ad is no longer in use. 
	@param initScan should be non-zero on first call to initialize the iterator
	@return NULL on failure or when done iterating; the job ClassAd on success
*/
ClassAd *GetNextJob(int initScan);
/** Iterate over jobs in the queue which match the specified constraint.
	The caller MUST call FreeJobAd when the ad is no longer in use. 
*/
ClassAd *GetNextJobByConstraint(const char *constraint, int initScan);
/** De-allocate job ClassAd allocated by GetJobAd, GetJobAdByConstraint,
	GetNextJob, or GetNextJobByConstraint.
*/
void FreeJobAd(ClassAd *&ad);

/** Initiate transfer of job's initial checkpoint file (the executable).
	Follow with a call to SendSpoolFileBytes.
	@param filename Name of initial checkpoint file destination
	@return -1 on failure; 0 on success
*/
int SendSpoolFile(char *filename);
/** Actually transfer the initial checkpoint file (the executable).
	@param filename Name of initial checkpoint file source.
*/
int SendSpoolFileBytes(char *filename);

void WalkJobQueue(scan_func);

void InitQmgmt();
void InitJobQueue(const char *job_queue_name,int max_historical_logs);
void CleanJobQueue();
bool setQSock( ReliSock* rsock );
void unsetQSock();


int rusage_to_float(struct rusage, float *, float *);
int float_to_rusage(float, float, struct rusage *);

/* These are here for compatibility with old code which uses the PROC
   structure to ease porting.  Use of these functions is discouraged! */
#if defined(NEW_PROC)
int GetProc(int, int, PROC *);
#endif

bool Reschedule();

#define SetAttributeExpr(cl, pr, name, val) SetAttribute(cl, pr, name, val);
#define SetAttributeExprByConstraint(con, name, val) SetAttributeByConstraint(con, name, val);

#endif
