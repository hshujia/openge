/*********************************************************************
 *
 * command_bpipe.cpp: Execute a BPIPE command script
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 12 September 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial 
 * Purpose License. A copy of this license has been provided in 
 * the openge/ directory.
 *
 *********************************************************************/

#include "commands.h"

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include "../util/bpipe.h"
using namespace std;
namespace po = boost::program_options;

void BPipeCommand::getOptions()
{
    options.add_options()
    ("test,t", "Reads and checks a bpipe pipeline without actually running the commands.")
    ("print,p", "Print the commands that will be executed by the pipeline.")
    ("print_execution,x", "Print the execution structure of the pipeline.")
    ;
}

int BPipeCommand::runCommand()
{
    if(input_filenames.size() != 1 && input_filenames.size() != 2) {
        cerr << "One input bpipe script is required." << endl;
        exit(-1);
    }
    
    BPipe pipe;
    
    if(!pipe.load(input_filenames[0].c_str())) {
        cerr << "Error loading bpipe file " << input_filenames[0] << endl;
        exit(-1);
    }
    string input_filename;
    if(input_filenames.size() > 1)
        input_filename = input_filenames[1];
    if(!pipe.check(input_filename)) {
        cerr << "Parsing bpipe file " << input_filenames[0] << " failed." << endl;
        exit(-1);
    }
    
    if(!vm.count("test") && !vm.count("print") && !vm.count("print_execution")) {
        if(!pipe.execute()) {
            cerr << "Executing bpipe file " << input_filenames[0] << " failed." << endl;
            exit(-1);
        }
    }
    
    if(vm.count("print"))
        pipe.print();
    
    return 0;
}