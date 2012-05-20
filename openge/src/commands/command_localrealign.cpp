#include "commands.h"

#include <vector>
#include <string>
using namespace std;
namespace po = boost::program_options;

#include "../algorithms/local_realignment.h"
#include "../algorithms/file_writer.h"
#include "../algorithms/file_reader.h"

void LocalRealignCommand::getOptions()
{ 
    options.add_options()
    ("out,o", po::value<string>()->default_value("stdout"), "Output filename. Omit for stdout.")
    ("reference,R", po::value<string>(), "Reference genome (FASTA format)")
    ("intervals,L", po::value<string>(), "Intervals file")
    ;
}

int LocalRealignCommand::runCommand()
{
    FileReader reader;
    LocalRealignment local_realignment;
    FileWriter writer;
    
    reader.addSink(&local_realignment);
    local_realignment.addSink(&writer);

    reader.addFiles(input_filenames);

    writer.setFilename(vm["out"].as<string>());

    local_realignment.verbose = verbose;
    
    if(vm.count("reference") != 1) {
        cerr << "One FASTA reference file is required." << endl;
        return -1;
    } else {
        local_realignment.setReferenceFilename(vm["reference"].as<string>());
        local_realignment.setIntervalsFilename(vm["intervals"].as<string>());
    }
    
    if(input_filenames.size() != 1) {
        cerr << "One input file is required. You supplied " << input_filenames.size() << endl;
        return -1;
    }

    reader.runChain();

    return 0;
}