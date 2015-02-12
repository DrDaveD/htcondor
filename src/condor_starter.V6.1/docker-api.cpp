#include "condor_common.h"

#include "env.h"
#include "my_popen.h"
#include "CondorError.h"
#include "condor_config.h"
#include "condor_classad.h"
#include "condor_daemon_core.h"

#include "docker-api.h"

static bool add_env_to_args_for_docker(ArgList &runArgs, const Env &env);
static bool add_docker_arg(ArgList &runArgs);

//
// Because we fork before calling docker, we don't actually
// care if the image is stored locally or not (except to the extent that
// remote image pull violates the principle of least astonishment).
//
int DockerAPI::run(
	const std::string & containerName,
	const std::string & imageID,
	const std::string & command,
	const ArgList & args,
	const Env & env,
	const std::string & sandboxPath,
	int & pid,
	int * childFDs,
	int * cpuAffinity,
	int memoryInMB,
	CondorError & /* err */ )
{
	ArgList runArgs;
	// Handle the special case for sudo.
	if(! add_docker_arg( runArgs )) { return -1; }
	runArgs.AppendArg( "run" );
	runArgs.AppendArg( "--tty" );

	//
	// Configure resource limits.
	//

	//
	// The --cpu-shares option is useless to us, since it sets a proportional
	// share, not a limit.  Instead, use --cpuset, if a CPU set is available.
	//
	if( cpuAffinity != NULL ) {
		std::string cpuSetArgument = "--cpuset=";
		for( int i = 1; i < cpuAffinity[0]; ++i ) {
			dprintf( D_FULLDEBUG, "Found CPU: %d\n", cpuAffinity[i] );
			formatstr( cpuSetArgument, "%s%d,", cpuSetArgument.c_str(), cpuAffinity[i] );
		}
		cpuSetArgument.erase( cpuSetArgument.length() - 1 );
		runArgs.AppendArg( cpuSetArgument );
	}

	if( memoryInMB != 0 ) {
		std::string memorySizeArgument;
		formatstr( memorySizeArgument, "--memory=%dm", memoryInMB );
		runArgs.AppendArg( memorySizeArgument );
	}

	// Let's not unnecessarily escalate privileges, here.
#ifdef WIN32
#else
	runArgs.AppendArg( "--user" );
	runArgs.AppendArg( geteuid() );
#endif

	runArgs.AppendArg( "--name" );
	runArgs.AppendArg( containerName );

	if(! add_env_to_args_for_docker( runArgs, env )) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to pass enviroment to docker.\n" );
		return -8;
	}

	// Map the external sanbox to the internal sandbox.
	runArgs.AppendArg( "--volume" );
	runArgs.AppendArg( sandboxPath + ":" + sandboxPath );

	// Start in the sandbox.
	runArgs.AppendArg( "--workdir" );
	runArgs.AppendArg( sandboxPath );

	// Run the command with its arguments in the image.
	runArgs.AppendArg( imageID );
	runArgs.AppendArg( command );
	runArgs.AppendArgsFromArgList( args );

	MyString displayString;
	runArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	FamilyInfo fi;
	fi.max_snapshot_interval = param_integer( "PID_SNAPSHOT_INTERVAL", 15 );
	int childPID = daemonCore->Create_Process( runArgs.GetArg(0), runArgs,
		PRIV_UNKNOWN, 1, FALSE, FALSE, NULL, sandboxPath.c_str(),
		& fi, NULL, childFDs );

	if( childPID == FALSE ) {
		dprintf( D_ALWAYS | D_FAILURE, "Create_Process() failed.\n" );
		return -1;
	}
	pid = childPID;

	// We assume, for now, that docker run will not return 0 unless it
	// started a container (even if that container immediately fails).
	// We detect that case in DockerProc::JobReaper, so we don't have
	// to do it here.
	return 0;
}

int DockerAPI::rm( const std::string & containerName, CondorError & /* err */ ) {

	ArgList rmArgs;
	if(! add_docker_arg( rmArgs )) { return -1; }
	rmArgs.AppendArg( "rm" );
	rmArgs.AppendArg( containerName.c_str() );

	MyString displayString;
	rmArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	// Read from Docker's combined output and error streams.
	FILE * dockerResults = my_popen( rmArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -2;
	}

	// On a success, Docker writes the successful identifier(s) -- ID or
	// name -- back out.
	char buffer[1024];
	if( NULL == fgets( buffer, 1024, dockerResults ) ) {
		if( errno ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to read results from '%s': '%s' (%d)\n", displayString.c_str(), strerror( errno ), errno );
		} else {
			dprintf( D_ALWAYS | D_FAILURE, "'%s' returned nothing.\n", displayString.c_str() );
		}
		my_pclose( dockerResults );
		return -3;
	}

	int length = strlen( buffer );
	if( length < 1 || strncmp( buffer, containerName.c_str(), length - 1 ) != 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "Docker remove failed, printing first few lines of output.\n" );
		dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		while( NULL != fgets( buffer, 1024, dockerResults ) ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		}
		my_pclose( dockerResults );
		return -4;
	}

	my_pclose( dockerResults );
	return 0;
}

int DockerAPI::version( std::string & version, CondorError & /* err */ ) {

	ArgList versionArgs;
	if(! add_docker_arg( versionArgs )) { return -1; }
	versionArgs.AppendArg( "-v" );

	MyString displayString;
	versionArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: '%s'.\n", displayString.c_str() );

	FILE * dockerResults = my_popen( versionArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -2;
	}

	char buffer[1024];
	if( NULL == fgets( buffer, 1024, dockerResults ) ) {
		if( errno ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to read results from '%s': '%s' (%d)\n", displayString.c_str(), strerror( errno ), errno );
		} else {
			dprintf( D_ALWAYS | D_FAILURE, "'%s' returned nothing.\n", displayString.c_str() );
		}
		my_pclose( dockerResults );
		return -3;
	}

	int exitCode = my_pclose( dockerResults );
	if( exitCode != 0 ) {
		dprintf( D_ALWAYS, "'%s' did not exit successfully (code %d); the first line of output was '%s'.\n", displayString.c_str(), exitCode, buffer );
		return -4;
	}

	unsigned end = strlen(buffer) - 1;
	if( buffer[end] == '\n' ) { buffer[end] = '\0'; }
	version = buffer;

	return 0;
}

int DockerAPI::detect( std::string & version, CondorError & err ) {
    if( DockerAPI::version( version, err ) != 0 ) {
        dprintf( D_ALWAYS | D_FAILURE, "DockerAPI::detect() failed to detect the Docker version; assuming absent.\n" );
        return -4;
    }

	ArgList infoArgs;
	if(! add_docker_arg( infoArgs )) { return -1; }
	infoArgs.AppendArg( "info" );

	MyString displayString;
	infoArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: '%s'.\n", displayString.c_str() );

	FILE * dockerResults = my_popen( infoArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -2;
	}

	// The results of 'docker info' may be useful for debugging.
	char buffer[1024];
	std::vector< std::string > output;
	while( fgets( buffer, 1024, dockerResults ) != NULL ) {
		unsigned end = strlen(buffer) - 1;
		if( buffer[end] == '\n' ) { buffer[end] = '\0'; }
		output.push_back( buffer );
	}
	for( unsigned i = 0; i < output.size(); ++i ) {
		dprintf( D_FULLDEBUG, "[docker info] %s\n", output[i].c_str() );
	}

	int exitCode = my_pclose( dockerResults );
	if( exitCode != 0 ) {
		dprintf( D_ALWAYS, "'%s' did not exit successfully (code %d); the first line of output was '%s'.\n", displayString.c_str(), exitCode, output[0].c_str() );
		return -3;
	}

	return 0;
}

int DockerAPI::inspect( const std::string & containerName, ClassAd * dockerAd, CondorError & /* err */ ) {
	if( dockerAd == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "dockerAd is NULL.\n" );
		return -2;
	}

	ArgList inspectArgs;
	if(! add_docker_arg( inspectArgs )) { return -1; }
	inspectArgs.AppendArg( "inspect" );
	inspectArgs.AppendArg( "--format" );
	StringList formatElements(	"ContainerId=\"{{.Id}}\" "
								"Pid={{.State.Pid}} "
								"ContainerName=\"{{.Name}}\" "
								"Running={{.State.Running}} "
								"ExitCode={{.State.ExitCode}} "
								"StartedAt=\"{{.State.StartedAt}}\" "
								"FinishedAt=\"{{.State.FinishedAt}}\" " );
	char * formatArg = formatElements.print_to_delimed_string( "\n" );
	inspectArgs.AppendArg( formatArg );
	free( formatArg );
	inspectArgs.AppendArg( containerName );

	MyString displayString;
	inspectArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	FILE * dockerResults = my_popen( inspectArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Unable to run '%s'.\n", displayString.c_str() );
		return -6;
	}

	// If the output isn't exactly formatElements.number() lines long,
	// something has gone wrong and we'll at least be able to print out
	// the error message(s).
	char buffer[1024];
	std::vector<std::string> correctOutput(formatElements.number());
	for( int i = 0; i < formatElements.number(); ++i ) {
		if( fgets( buffer, 1024, dockerResults ) != NULL ) {
			correctOutput[i] = buffer;
		}
	}
	my_pclose( dockerResults );

	int attrCount = 0;
	for( int i = 0; i < formatElements.number(); ++i ) {
		if( correctOutput[i].empty() || dockerAd->Insert( correctOutput[i].c_str() ) == FALSE ) {
			break;
		}
		++attrCount;
	}

	if( attrCount != formatElements.number() ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to create classad from Docker output (%d).  Printing up to the first %d (nonblank) lines.\n", attrCount, formatElements.number() );
		for( int i = 0; i < formatElements.number() && ! correctOutput[i].empty(); ++i ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", correctOutput[i].c_str() );
		}
		return -4;
	}

	dprintf( D_FULLDEBUG, "docker inspect printed:\n" );
	for( int i = 0; i < formatElements.number() && ! correctOutput[i].empty(); ++i ) {
		dprintf( D_FULLDEBUG, "\t%s", correctOutput[i].c_str() );
	}

	return 0;
}

//
// In most cases we can't invoke docker directly because of it will be
// privileged.  Instead, DOCKER will be defined as 'sudo docker' or
// 'sudo /path/to/docker', so we need to recognise this as two arguments
// and do the right thing.
//
static bool add_docker_arg( ArgList & args ) {
	std::string docker;
	if( ! param( docker, "DOCKER" ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DOCKER is undefined.\n" );
		return false;
	}
	const char * pdocker = docker.c_str();
	if( starts_with( docker, "sudo " ) ) {
		args.AppendArg( "/usr/bin/sudo" );
		pdocker += 4;
		while( isspace( *pdocker ) ) { ++pdocker; }
		if(! *pdocker) {
			dprintf( D_ALWAYS | D_FAILURE, "DOCKER is defined as '%s' which is not valid.\n", docker.c_str() );
			return false;
		}
	}
	args.AppendArg( pdocker );
	return true;
}

static bool docker_add_env_walker (void*pv, const MyString &var, const MyString &val) {
	ArgList* runArgs = (ArgList*)pv;
	MyString arg;
	arg.reserve_at_least(var.length() + val.length() + 2);
	arg = var;
	arg += "=";
	arg += val;
	runArgs->AppendArg("-e");
	runArgs->AppendArg(arg);
	return true; // true to keep iterating
}

// helper function, convert Env into arguments for docker run.
// essentially this means adding each as an argument of the form -e name=value
bool add_env_to_args_for_docker(ArgList &runArgs, const Env &env)
{
	dprintf(D_ALWAYS | D_VERBOSE, "adding %d environment vars to docker args\n", env.Count());
	env.Walk(
#if 1 // sigh
		docker_add_env_walker,
#else
		// use a lamda to walk the environment and add each entry as
		[](void*pv, const MyString &var, const MyString &val) -> bool {
			ArgList& runArgs = *(ArgList*)pv;
			runArgs.AppendArg("-e");
			MyString arg(var); arg += "="; arg += val;
			runArgs.AppendArg(arg);
			return true; // true to keep iterating
		},
#endif
		&runArgs
	);

	return true;
}

int DockerAPI::kill( const std::string & containerName, int signal, CondorError & /* err */ ) {

	ArgList killArgs;
	if(! add_docker_arg( killArgs )) { return -1; }
	killArgs.AppendArg( "kill" );
	std::string signalArgument;
	formatstr( signalArgument, "--signal=%d", signal );
	killArgs.AppendArg( signalArgument );
	killArgs.AppendArg( containerName );

	MyString displayString;
	killArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	// Read from Docker's combined output and error streams.
	FILE * dockerResults = my_popen( killArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -2;
	}

	// Docker writes the container ID back out on success.  (It won't fail
	// to send a signal to a container even if that container isn't running,
	// but it will complain if the container doesn't exist.)
	char buffer[1024];
	if( NULL == fgets( buffer, 1024, dockerResults ) ) {
		if( errno ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to read results from '%s': '%s' (%d)\n", displayString.c_str(), strerror( errno ), errno );
		} else {
			dprintf( D_ALWAYS | D_FAILURE, "'%s' returned nothing.\n", displayString.c_str() );
		}
		my_pclose( dockerResults );
		return -3;
	}

	int length = strlen( buffer );
	if( length < 1 || strncmp( buffer, containerName.c_str(), length - 1 ) != 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "Docker kill failed, printing first few lines of output.\n" );
		dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		while( NULL != fgets( buffer, 1024, dockerResults ) ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		}
		my_pclose( dockerResults );
		return -4;
	}

	my_pclose( dockerResults );
	return 0;
}
