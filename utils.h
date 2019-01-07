/*
 *      Software License Agreement (BSD License)
 *
 *      Copyright (c) 2010-2011, Calvin Tee (collectskin.com)
 *      All rights reserved.
 *
 *      2011-12-25 Updated by Sudarshan S. Chawathe (chaw@eip10.org).
 *                 Comments and minor changes.
 *
 *      Redistribution and use in source and binary forms, with or without
 *      modification, are permitted provided that the following conditions are
 *      met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following disclaimer
 *        in the documentation and/or other materials provided with the
 *        distribution.
 *      * Neither the name of the  nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 *      THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *      "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *      LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *      A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *      OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *      SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *      LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *      DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *      THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *      (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *      OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <fuse.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>
#include <queue>
#include <vector>
#include <map>
#include <unistd.h>

using namespace std;

struct fileCache{
    time_t timestamp;
    string statOutput;
};

queue<string> exec_command(const string&);
vector<string> make_array(const string&, const string&);

string& string_replacer(string&, const string&, const string&);

vector<string> make_array(const string& data, const string& delimiters = " "){
    vector<string> result;
    if (data.size() < 1) return result;
    //string delimiters = " ";
    string::size_type lastPos = data.find_first_not_of(delimiters, 0);
    string::size_type pos     = data.find_first_of(delimiters, lastPos);
    while (string::npos != pos || string::npos != lastPos)
    {
        result.push_back(data.substr(lastPos, pos - lastPos));
        lastPos = data.find_first_not_of(delimiters, pos);
        if (lastPos != string::npos)
            pos = data.find_first_of(delimiters, lastPos);
        else
            break;
    }
    return result;
}

/**
   Replace all instances of string s1 with string s2 in the given
   string s, which is modified in-place.

   The work proceeds down the string and skips over the text
   introduced by the replacment.

   @param source the string s to be modified.
   @param find the string s2, i.e., the pattern to be replaced.
   @param replace the string s3, i.e., the replacement for s2.
 */
string& string_replacer(string &source, const string& find, const string& replace) {
    size_t j = 0;
    for ( ; (j = source.find(find,j)) != string::npos ; ) {
	source.replace( j, find.length(), replace );
        j = j + replace.length();
    }
    return source;
}

int xtoi(const char* xs, unsigned int* result)
{
 size_t szlen = strlen(xs);
 int i, xv, fact;

 if (szlen > 0)
 {
  // Converting more than 32bit hexadecimal value?
  if (szlen>8) return 2; // exit

  // Begin conversion here
  *result = 0;
  fact = 1;

  // Run until no more character to convert
  for(i=szlen-1; i>=0 ;i--)
  {
   if (isxdigit(*(xs+i)))
   {
    if (*(xs+i)>=97)
    {
     xv = ( *(xs+i) - 97) + 10;
    }
    else if ( *(xs+i) >= 65)
    {
     xv = (*(xs+i) - 65) + 10;
    }
    else
    {
     xv = *(xs+i) - 48;
    }
    *result += (xv * fact);
    fact *= 16;
   }
   else
   {
    // Conversion was abnormally terminated
    // by non hexadecimal digit, hence
    // returning only the converted with
    // an error value 4 (illegal hex character)
    return 4;
   }
  }
 }

 // Nothing to convert
 return 1;
}

/**
   Execute the given command string as a shell command.

   @param command the string to be executed as a command.
 */
queue<string> exec_command(const string& command)
{
    cout << "--*-- " << "exec_command: "  << command << "\n";
    queue<string> output;
    FILE *fp = popen(command.c_str(), "r" );

    char buff[1000];
    string tmp_string;
    while ( fgets( buff, sizeof buff, fp ) != NULL && !feof(fp) )
    {
        tmp_string.assign(buff);
        while ((tmp_string.size() > 0 &&
               tmp_string[tmp_string.size() - 1] == '\n') ||
               tmp_string[tmp_string.size() - 1] == '\r') {
          tmp_string.erase(tmp_string.size()-1);
        }
        output.push(tmp_string);
    }

    pclose( fp );

    return output;
}

