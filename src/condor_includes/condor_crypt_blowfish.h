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


#ifndef CONDOR_CRYPTO_BLOWFISH_H
#define CONDOR_CRYPTO_BLOWFISH_H

#ifdef HAVE_EXT_OPENSSL

#include "condor_common.h"
#include "condor_crypt.h"          // base class

class Condor_Crypt_Blowfish : public Condor_Crypt_Base {

 public:
    Condor_Crypt_Blowfish() {}
    ~Condor_Crypt_Blowfish() {}

    bool encrypt(Condor_Crypto_State *s,
                 const unsigned char * input,
                 int          input_len, 
                 unsigned char *&      output, 
                 int&         output_len);

    bool decrypt(Condor_Crypto_State *s,
                 const unsigned char * input,
                 int          input_len, 
                 unsigned char *&      output, 
                 int&         output_len);
};

#endif

#endif
