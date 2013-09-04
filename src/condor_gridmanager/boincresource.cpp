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
#include "string_list.h"

#include "boincresource.h"
#include "boincjob.h"
#include "gridmanager.h"

#ifdef WIN32
	#include <sys/types.h> 
	#include <sys/timeb.h>
#endif

using std::list;
using std::vector;
using std::set;

#define DEFAULT_MAX_SUBMITTED_JOBS_PER_RESOURCE		100
#define DEFAULT_LEASE_DURATION		(6 * 60 * 60)
#define SUBMIT_DELAY				2


int BoincResource::gahpCallTimeout = 300;	// default value

#define HASH_TABLE_SIZE			500

HashTable <HashKey, BoincResource *>
    BoincResource::ResourcesByName( HASH_TABLE_SIZE,
									hashFunction );

enum BatchSubmitStatus {
	BatchUnsubmitted,
	BatchMaybeSubmitted,
	BatchSubmitting,
	BatchSubmitted,
	BatchFailed,
};

struct BoincBatch {
	std::string m_batch_name;
	BatchSubmitStatus m_submit_status;
	time_t m_lease_time;
	time_t m_last_insert;
	std::string m_error_message;
	std::set<BoincJob *> m_jobs;
	std::set<BoincJob *> m_jobs_ready;
};

BoincResource *BoincResource::FindOrCreateResource( const char *resource_name,
													const char *authenticator )
{
	int rc;
	BoincResource *resource = NULL;

	const char *hash_name = HashName( resource_name, authenticator );
	ASSERT(hash_name);

	rc = ResourcesByName.lookup( HashKey( hash_name ), resource );
	if ( rc != 0 ) {
		resource = new BoincResource( resource_name, authenticator );
		ASSERT(resource);
		if ( resource->Init() == false ) {
			delete resource;
			resource = NULL;
		} else {
			ResourcesByName.insert( HashKey( hash_name ), resource );
		}
	} else {
		ASSERT(resource);
	}

	return resource;
}

BoincResource::BoincResource( const char *resource_name,
							  const char *authenticator )
	: BaseResource( resource_name )
{
	initialized = false;
	gahp = NULL;
	m_statusGahp = NULL;
	m_leaseGahp = NULL;
	m_activeLeaseBatch = NULL;
	m_submitGahp = NULL;
	m_activeSubmitBatch = NULL;

//	hasLeases = true;
//	m_hasSharedLeases = true;
//	m_defaultLeaseDuration = 6 * 60 * 60;

	m_serviceUri = strdup( resource_name );
	m_authenticator = strdup( authenticator );

	m_leaseTid = daemonCore->Register_Timer( 0,
							(TimerHandlercpp)&BoincResource::UpdateBoincLeases,
							"BoincResource::UpdateBoincLeases", (Service*)this );

	m_submitTid = daemonCore->Register_Timer( 0,
							(TimerHandlercpp)&BoincResource::DoBatchSubmits,
							"BoincResource::DoBatchSubmits", (Service*)this );
}

BoincResource::~BoincResource()
{
	daemonCore->Cancel_Timer( m_leaseTid );
	daemonCore->Cancel_Timer( m_submitTid );

	free( m_serviceUri );
	free( m_authenticator );

	ResourcesByName.remove( HashKey( HashName( resourceName, m_authenticator ) ) );

	delete gahp;
	delete m_statusGahp;
	delete m_leaseGahp;
	delete m_submitGahp;

	while ( !m_batches.empty() ) {
		delete m_batches.front();
		m_batches.pop_front();
	}
}

bool BoincResource::Init()
{
	if ( initialized ) {
		return true;
	}

		// TODO This assumes that at least one BoincJob has already
		// initialized the gahp server. Need a better solution.
	std::string gahp_name;
	formatstr( gahp_name, "BOINC" );

	gahp = new GahpClient( gahp_name.c_str() );

	gahp->setNotificationTimerId( pingTimerId );
	gahp->setMode( GahpClient::normal );
	gahp->setTimeout( gahpCallTimeout );
	gahp->setBoincResource( this );

	m_statusGahp = new GahpClient( gahp_name.c_str() );

	StartBatchStatusTimer();

	m_statusGahp->setNotificationTimerId( BatchPollTid() );
	m_statusGahp->setMode( GahpClient::normal );
	m_statusGahp->setTimeout( gahpCallTimeout );
	m_statusGahp->setBoincResource( this );

	m_leaseGahp = new GahpClient( gahp_name.c_str() );

	m_leaseGahp->setNotificationTimerId( m_leaseTid );
	m_leaseGahp->setMode( GahpClient::normal );
	m_leaseGahp->setTimeout( gahpCallTimeout );
	m_leaseGahp->setBoincResource( this );

	m_submitGahp = new GahpClient( gahp_name.c_str() );

	m_submitGahp->setNotificationTimerId( m_submitTid );
	m_submitGahp->setMode( GahpClient::normal );
	m_submitGahp->setTimeout( gahpCallTimeout );
	m_submitGahp->setBoincResource( this );

	char* pool_name = param( "COLLECTOR_HOST" );
	if ( pool_name ) {
		StringList collectors( pool_name );
		free( pool_name );
		pool_name = collectors.print_to_string();
	}
	if ( !pool_name ) {
		pool_name = strdup( "NoPool" );
	}

	free( pool_name );

	initialized = true;

	Reconfig();

	return true;
}

void BoincResource::Reconfig()
{
	BaseResource::Reconfig();

	gahp->setTimeout( gahpCallTimeout );
	m_statusGahp->setTimeout( gahpCallTimeout );
	m_leaseGahp->setTimeout( gahpCallTimeout );
	// TODO need longer timeout for submission
	m_submitGahp->setTimeout( gahpCallTimeout );
}

const char *BoincResource::ResourceType()
{
	return "boinc";
}

const char *BoincResource::HashName( const char *resource_name,
									 const char *authenticator )
{
	static std::string hash_name;

	formatstr( hash_name, "boinc %s %s", resource_name, authenticator );

	return hash_name.c_str();
}

void BoincResource::RegisterJob( BaseJob *base_job )
{
	BoincJob* job = dynamic_cast<BoincJob*>( base_job );
	ASSERT( job );

	int job_lease;
	if ( m_sharedLeaseExpiration == 0 ) {
		if ( job->jobAd->LookupInteger( ATTR_JOB_LEASE_EXPIRATION, job_lease ) ) {
			m_sharedLeaseExpiration = job_lease;
		}
	} else {
		if ( job->jobAd->LookupInteger( ATTR_JOB_LEASE_EXPIRATION, job_lease ) ) {
			job->UpdateJobLeaseSent( m_sharedLeaseExpiration );
		}
	}

	// TODO should we also reset the timer if this job has a shorter
	//   lease duration than all existing jobs?
	if ( m_sharedLeaseExpiration == 0 ) {
		daemonCore->Reset_Timer( updateLeasesTimerId, 0 );
	}

	BaseResource::RegisterJob( job );
}

void BoincResource::UnregisterJob( BaseJob *base_job )
{
	BoincJob *job = dynamic_cast<BoincJob*>( base_job );
	ASSERT( job );

	BaseResource::UnregisterJob( job );

	for ( list<BoincBatch *>::iterator batch_itr = m_batches.begin();
		  batch_itr != m_batches.end(); batch_itr++ ) {
		for ( set<BoincJob *>::iterator job_itr = (*batch_itr)->m_jobs.begin();
			  job_itr != (*batch_itr)->m_jobs.end(); job_itr++ ) {

			if ( (*job_itr) == job ) {
				(*batch_itr)->m_jobs.erase( (*job_itr) );
				break;
			}
		}
		if ( (*batch_itr)->m_jobs.empty() ) {
			// This batch is empty, clean it up and delete it
			if ( m_activeLeaseBatch == (*batch_itr) ) {
				// We have a lease command running on this batch
				// Purge it and check other batches
				m_leaseGahp->purgePendingRequests();
				m_activeLeaseBatch = NULL;
				daemonCore->Reset_Timer( m_leaseTid, 0 );
			}
			if ( m_activeSubmitBatch == (*batch_itr) ) {
				// We have a submit command running on this batch
				// Purge it and check other batches
				m_submitGahp->purgePendingRequests();
				m_activeSubmitBatch = NULL;
				daemonCore->Reset_Timer( m_submitTid, 0 );
			}
			delete (*batch_itr);
			m_batches.erase( batch_itr );
			break;
		}
	}
}

const char *BoincResource::GetHashName()
{
	return HashName( resourceName, m_authenticator );
}

void BoincResource::PublishResourceAd( ClassAd *resource_ad )
{
	BaseResource::PublishResourceAd( resource_ad );
}

bool BoincResource::JoinBatch( BoincJob *job, std::string &batch_name,
							   std::string & /*error_str*/ )
{
	if ( !batch_name.empty() ) {
		BoincBatch *batch = NULL;
		for ( list<BoincBatch *>::iterator batch_itr = m_batches.begin();
			  batch_itr != m_batches.end(); batch_itr++ ) {
			if ( (*batch_itr)->m_batch_name == batch_name ) {
				batch = *batch_itr;
				break;
			}
		}

		if ( batch == NULL ) {
			batch = new BoincBatch();
			batch->m_batch_name = batch_name;
			batch->m_lease_time = 0;
			batch->m_submit_status = BatchMaybeSubmitted;
			m_batches.push_back( batch );
		}

		// TODO update batch->m_lease_time based in job's lease expiration
		//   time, if appropriate
		if ( batch->m_submit_status == BatchUnsubmitted ) {
			batch->m_submit_status = BatchMaybeSubmitted;
		}
		if ( batch->m_submit_status == BatchMaybeSubmitted && !job->remoteState.empty() ) {
			batch->m_submit_status = BatchSubmitted;
		}
		batch->m_last_insert = time(NULL);
		batch->m_jobs.insert( job );
		return true;
	} else {

		BoincBatch *batch = NULL;
		for ( list<BoincBatch *>::iterator batch_itr = m_batches.begin();
			  batch_itr != m_batches.end(); batch_itr++ ) {
			// Assume all jobs in a cluster belong in the same boinc batch
			// But we can't add this job to a batch that's already been
			// submitted.
			if ( (*(*batch_itr)->m_jobs.begin())->procID.cluster == job->procID.cluster && (*batch_itr)->m_submit_status == BatchUnsubmitted ) {
				batch = *batch_itr;
				break;
			}
		}

		if ( batch == NULL ) {
			batch = new BoincBatch();
			// This batch naming scheme assumes all jobs in a cluster
			// should go into the same boinc batch.
			formatstr( batch->m_batch_name, "condor#%s#%d#%d", ScheddName,
					   job->procID.cluster, (int)time(NULL) );
			batch->m_lease_time = 0;
			batch->m_submit_status = BatchUnsubmitted;
			m_batches.push_back( batch );
		}

		batch->m_last_insert = time(NULL);
		batch->m_jobs.insert( job );
		batch_name = batch->m_batch_name;
		return true;
	}
	// TODO For an unsubmitted batch, may need to update state to indicate
	//   it's not ready to submit yet.
	// TODO Need to check error recovery
	//   Ensure that all jobs claiming to be in a batch were included in
	//   submission
	//   Could query BOINC server to enforce
}

BoincSubmitResponse BoincResource::Submit( BoincJob *job,
										   std::string &error_str )
{
	if ( job->remoteBatchName == NULL ) {
		error_str = "Job has no batch name";
		return BoincSubmitFailure;
	}

	BoincBatch *batch = NULL;
	for ( list<BoincBatch *>::iterator batch_itr = m_batches.begin();
		  batch_itr != m_batches.end(); batch_itr++ ) {
		if ( (*batch_itr)->m_batch_name == job->remoteBatchName ) {
			batch = *batch_itr;
			break;
		}
	}
	if ( batch == NULL ) {
		error_str = "BoincBatch not found";
		return BoincSubmitFailure;
	}

	if ( batch->m_submit_status == BatchFailed ) {
		error_str = batch->m_error_message;
		return BoincSubmitFailure;
	}
	if ( batch->m_submit_status == BatchSubmitted ) {
		return BoincSubmitSuccess;
	}
	if ( batch->m_submit_status == BatchSubmitting ) {
		// If we've started submitting, avoid any checks below
		return BoincSubmitWait;
	}
	// TODO any other cases where we're not waiting for submission?

	batch->m_jobs_ready.insert( job );
	// If we in BatchMaybeSubmitted and m_error_message is set, then
	// the batch query failed and we should return the error message
	// to the job. We don't return BoincSubmitFailure, because that
	// indicates the batch has not been submitted and no cancelation
	// is required.
	if ( batch->m_submit_status == BatchMaybeSubmitted ) {
		error_str = batch->m_error_message;
	}
	if ( BatchReadyToSubmit( batch ) ) {
		daemonCore->Reset_Timer( m_submitTid, 0 );
	}
	return BoincSubmitWait;
}

bool BoincResource::BatchReadyToSubmit( BoincBatch *batch, int *delay )
{
	if ( time(NULL) < batch->m_last_insert + SUBMIT_DELAY ) {
		if ( delay ) {
			*delay = (batch->m_last_insert + SUBMIT_DELAY) - time(NULL);
		}
		return false;
	}
	if ( batch->m_jobs != batch->m_jobs_ready ) {
		if ( delay ) {
			*delay = TIMER_NEVER;
		}
		return false;
	}
	return true;
}

void BoincResource::DoPing( time_t& ping_delay, bool& ping_complete,
							bool& ping_succeeded )
{
	int rc;

	if ( gahp->isStarted() == false ) {
		dprintf( D_ALWAYS,"gahp server not up yet, delaying ping\n" );
		ping_delay = 5;
		return;
	}

	ping_delay = 0;

	rc = gahp->boinc_ping();
	
	if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
		ping_complete = false;
	} else if ( rc == GLOBUS_GRAM_PROTOCOL_ERROR_CONTACTING_JOB_MANAGER ) {
		ping_complete = true;
		ping_succeeded = false;
	} else {
		ping_complete = true;
		ping_succeeded = true;
	}
}


BoincResource::BatchStatusResult BoincResource::StartBatchStatus()
{
	m_statusBatches.clearAll();
	for ( std::list<BoincBatch*>::iterator itr = m_batches.begin();
		  itr != m_batches.end(); itr++ ) {
		if ( (*itr)->m_submit_status == BatchSubmitted ||
			 (*itr)->m_submit_status == BatchMaybeSubmitted ) {
			m_statusBatches.append( (*itr)->m_batch_name.c_str() );
		}
	}

	return FinishBatchStatus();
}

BoincResource::BatchStatusResult BoincResource::FinishBatchStatus()
{
	if ( m_statusBatches.isEmpty() ) {
		return BSR_DONE;
	}

	GahpClient::BoincQueryResults results;
	int rc = m_statusGahp->boinc_query_batches( m_statusBatches, results );
	if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
		return BSR_PENDING;
	}
	if ( rc != 0 ) {
		// TODO Save error string for use in hold messages?
		dprintf( D_ALWAYS, "Error getting BOINC status: %s\n",
				 m_statusGahp->getErrorString() );

		// If this error looks like it would affect a submit command,
		// notify all jobs in BatchMaybeSubmitted state.
		if ( !strstr( m_statusGahp->getErrorString(), "no batch named" ) ) {
			for ( std::list<BoincBatch*>::iterator batch_itr = m_batches.begin();
				  batch_itr != m_batches.end(); batch_itr++ ) {
				if ( (*batch_itr)->m_submit_status != BatchMaybeSubmitted ||
					 !(*batch_itr)->m_error_message.empty() ) {
					continue;
				}
				(*batch_itr)->m_error_message = m_statusGahp->getErrorString();
				for ( set<BoincJob *>::iterator job_itr = (*batch_itr)->m_jobs.begin();
					  job_itr != (*batch_itr)->m_jobs.end(); job_itr++ ) {
					(*job_itr)->SetEvaluateState();
				}
			}
		}

		m_statusBatches.clearAll();
		return BSR_ERROR;
	}

	// TODO We're not detecting missing jobs from batch results
	//   Do we need to?
	m_statusBatches.rewind();
	const char *batch_name;

	// Iterate over the batches in the response
	for ( GahpClient::BoincQueryResults::iterator i = results.begin(); i != results.end(); i++ ) {
		batch_name = m_statusBatches.next();
		ASSERT( batch_name );

		// If we're in recovery, we may not know whether this batch has
		// been submitted.
		for ( list<BoincBatch *>::iterator batch_itr = m_batches.begin();
			  batch_itr != m_batches.end(); batch_itr++ ) {
			if ( (*batch_itr)->m_batch_name == batch_name && 
				 (*batch_itr)->m_submit_status == BatchMaybeSubmitted ) {

				if ( i->empty() ) {
					// Batch doesn't exist on sever
					// Consider it for submission
					(*batch_itr)->m_submit_status = BatchUnsubmitted;
					daemonCore->Reset_Timer( m_submitTid, 0 );
				} else {
					// Batch exists on server
					// Signal the jobs
					(*batch_itr)->m_submit_status = BatchSubmitted;
					for ( set<BoincJob *>::iterator job_itr = (*batch_itr)->m_jobs.begin();
						  job_itr != (*batch_itr)->m_jobs.end(); job_itr++ ) {
						(*job_itr)->SetEvaluateState();
					}
				}
				break;
			}
		}

		// Iterate over the jobs for this batch
		for ( GahpClient::BoincBatchResults::iterator j = i->begin(); j != i->end(); j++ ) {
			const char *job_id = strrchr( j->first.c_str(), '#' );
			if ( job_id == NULL ) {
				dprintf( D_ALWAYS, "Failed to find job id in '%s'\n", j->first.c_str() );
				continue;
			}
			job_id++;
			PROC_ID proc_id;
			if ( sscanf( job_id, "%d.%d", &proc_id.cluster, &proc_id.proc ) != 2 ) {
				dprintf( D_ALWAYS, "Failed to parse job id '%s'\n", j->first.c_str() );
				continue;
			}
			BaseJob *base_job;
			if ( BaseJob::JobsByProcId.lookup( proc_id, base_job ) == 0 ) {
				BoincJob *boinc_job = dynamic_cast<BoincJob*>( base_job );
				ASSERT( boinc_job != NULL );
				boinc_job->NewBoincState( j->second.c_str() );
			}
		}
	}

	m_statusBatches.clearAll();
	return BSR_DONE;
}

GahpClient * BoincResource::BatchGahp() { return m_statusGahp; }

void BoincResource::DoBatchSubmits()
{
dprintf(D_FULLDEBUG,"*** DoBatchSubmits()\n");
	int delay = TIMER_NEVER;

	if ( m_submitGahp == NULL || !m_submitGahp->isStarted() ) {
		dprintf( D_FULLDEBUG, "gahp server not up yet, delaying DoBatchSubmits\n" );
		daemonCore->Reset_Timer( m_submitTid, 5 );
		return;
	}

	for ( list<BoincBatch*>::iterator batch = m_batches.begin();
		  batch != m_batches.end(); batch++ ) {

		if ( (*batch)->m_submit_status == BatchMaybeSubmitted ||
			 (*batch)->m_submit_status == BatchSubmitted ||
			 (*batch)->m_submit_status == BatchFailed ) {
			continue;
		}

		// If we have an active submit command, skip to that batch
		int this_delay = TIMER_NEVER;
		if ( m_activeSubmitBatch != NULL ) {
			if ( *batch != m_activeSubmitBatch ) {
				continue;
			}
		} else if ( !BatchReadyToSubmit( *batch, &this_delay ) ) {
			if ( this_delay < delay ) {
				delay = this_delay;
			}
			continue;
		}

		if ( m_activeSubmitBatch == NULL ) {

			// Let's start submitting this batch
			int rc = m_submitGahp->boinc_submit( (*batch)->m_batch_name.c_str(),
												 (*batch)->m_jobs );
			if ( rc != GAHPCLIENT_COMMAND_PENDING ) {
				dprintf( D_ALWAYS, "New boinc_submit() didn't return PENDING!?: %s\n", m_submitGahp->getErrorString() );
				m_submitGahp->purgePendingRequests();
				// TODO What else should we do?
			} else {
				m_activeSubmitBatch = (*batch);
				delay = TIMER_NEVER;
				break; // or reset timer and return?
			}

		} else {
			// active submit command, check if it's done
			// TODO avoid overhead of recreating arguments for 
			//   already-submitted command
			int rc = m_submitGahp->boinc_submit( (*batch)->m_batch_name.c_str(),
												 (*batch)->m_jobs );
			if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
				// do nothing, wait for command to complete
				delay = TIMER_NEVER;
				break;
			}
			m_activeSubmitBatch = NULL;

			if ( rc == 0 ) {
				// success
				(*batch)->m_submit_status = BatchSubmitted;
			} else {
				dprintf( D_ALWAYS, "Failed to submit batch %s: %s\n",
						 (*batch)->m_batch_name.c_str(),
						 m_submitGahp->getErrorString() );
				(*batch)->m_submit_status = BatchFailed;
				(*batch)->m_error_message = m_submitGahp->getErrorString();
			}

			for ( set<BoincJob *>::iterator job = (*batch)->m_jobs.begin();
				  job != (*batch)->m_jobs.end(); job++ ) {

				(*job)->SetEvaluateState();
			}
			// Re-call this method immediately to check for other batches
			// to submit.
			delay = 0;
		}
	}

	daemonCore->Reset_Timer( m_submitTid, delay );
}

void BoincResource::UpdateBoincLeases()
{
dprintf(D_FULLDEBUG,"*** UpdateBoincLeases()\n");
	int delay = TIMER_NEVER;

	if ( m_leaseGahp == NULL || !m_leaseGahp->isStarted() ) {
		dprintf( D_FULLDEBUG, "gahp server not up yet, delaying UpdateBoincLeases\n" );
		daemonCore->Reset_Timer( m_leaseTid, 5 );
		return;
	}

	for ( list<BoincBatch*>::iterator batch = m_batches.begin();
		  batch != m_batches.end(); batch++ ) {

		if ( !(*batch)->m_submit_status == BatchSubmitted ) {
			continue;
		}

		// If we have an active lease update command, skip to that batch
		if ( m_activeLeaseBatch != NULL && *batch != m_activeLeaseBatch ) {
			continue;
		}

		if ( m_activeLeaseBatch == NULL ) {
			// Calculate how long until this batch's lease needs to be updated
			time_t this_delay = ( (*batch)->m_lease_time + (DEFAULT_LEASE_DURATION / 3) ) - time(NULL);
			if ( this_delay <= 0 ) {
				// lease needs to be updated
				time_t new_lease_time = time(NULL) + DEFAULT_LEASE_DURATION;
				int rc = m_leaseGahp->boinc_set_lease( (*batch)->m_batch_name.c_str(),
													   new_lease_time );
				if ( rc != GAHPCLIENT_COMMAND_PENDING ) {
					dprintf( D_ALWAYS, "New boinc_set_lease() didn't return PENDING!?: %s\n", m_leaseGahp->getErrorString() );
					m_leaseGahp->purgePendingRequests();
					// TODO What else should we do?
				} else {
					m_activeLeaseBatch = (*batch);
					m_activeLeaseTime = new_lease_time;
					delay = TIMER_NEVER;
					break; // or reset timer and return?
				}
			} else {
				if ( this_delay < delay ) {
					delay = this_delay;
				}
			}
		} else {
			// active lease command, check if it's done
			int rc = m_leaseGahp->boinc_set_lease( (*batch)->m_batch_name.c_str(),
												   m_activeLeaseTime );
			if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
				// do nothing, wait for command to complete
				break;
			}
			m_activeLeaseBatch = NULL;
			if ( rc == 0 ) {
				// success
				(*batch)->m_lease_time = m_activeLeaseTime;
				for ( set<BoincJob *>::iterator job = (*batch)->m_jobs.begin();
					  job != (*batch)->m_jobs.end(); job++ ) {

					(*job)->UpdateJobLeaseSent( m_activeLeaseTime );
				}
			} else {
				dprintf( D_ALWAYS, "Failed to set lease for batch %s: %s\n",
						 (*batch)->m_batch_name.c_str(),
						 m_leaseGahp->getErrorString() );
				// TODO What else do we do?
			}

		}
	}

	daemonCore->Reset_Timer( m_leaseTid, delay );
}

/*
void BoincResource::DoUpdateSharedLease( time_t& update_delay,
										 bool& update_complete,
										 bool& update_succeeded )
{
	// TODO Can we use BaseResource's existing code for lease updates?
	//  It assumes a separate lease per job or one lease for all jobs.
	//  We have a lease for each batch.
}
*/
