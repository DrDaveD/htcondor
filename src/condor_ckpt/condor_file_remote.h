
#ifndef CONDOR_FILE_REMOTE_H
#define CONDOR_FILE_REMOTE_H

#include "condor_file_basic.h"

/**
This class sends all I/O operations to a remotely opened file.
Notice that the class is only a few extensions to CondorFileBasic.
The only operations which are unique to this class are the methods
listed here.  Operations that are common to both local and remote
files should go in CondorFileBasic.
*/

class CondorFileRemote : public CondorFileBasic {
public:
	CondorFileRemote();

	virtual int read(int offset, char *data, int length);
	virtual int write(int offset, char *data, int length);

	virtual int fcntl( int cmd, int arg );
	virtual int ioctl( int cmd, int arg );
	virtual int ftruncate( size_t length );

	virtual int is_file_local();
};

#endif
