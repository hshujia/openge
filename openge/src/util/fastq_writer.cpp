/*********************************************************************
 *
 * fastq.cpp: A simple FASTQ writer.
 * Open Genomics Engine
 *
 * Author: Lee C. Baker, VBI
 * Last modified: 16 March 2012
 *
 *********************************************************************
 *
 * This file is released under the Virginia Tech Non-Commercial 
 * Purpose License. A copy of this license has been provided in 
 * the openge/ directory.
 *
 *********************************************************************/

#include <iostream>
#include <sstream>
#include <cassert>
#include "fastq_writer.h"

using namespace std;
using namespace BamTools;

FastqWriter::FastqWriter() 
: fwd_stream(&cout)
, rev_stream(&cout)
, orphan_stream(&cout)
, open(false)
{
    
}

bool FastqWriter::Open(const string& filename, const std::string& samHeaderText, const BamTools::RefVector& referenceSequences) {
    this->filename = filename;
    
    if(filename != "stdout") {
        fwd_file.open((filename + "_1.fastq").c_str(), ios::out);
        
        if(fwd_file.fail()) {
            cerr << "Failed to open FASTQ forward output file " << filename << endl;
            return false;
        }
        fwd_stream = &fwd_file;
        
        rev_file.open((filename + "_2.fastq").c_str(), ios::out);
        
        if(rev_file.fail()) {
            cerr << "Failed to open FASTQ reverse output file " << filename << endl;
            return false;
        }
        rev_stream = &rev_file;
        
        orphan_file.open((filename + ".fastq").c_str(), ios::out);
        
        if(orphan_file.fail()) {
            cerr << "Failed to open FASTQ orphan output file " << filename << endl;
            return false;
        }
        orphan_stream = &orphan_file;
    }

    open = true;
    
    return true;
}

bool FastqWriter::Close() {
    
    for(map<string, fastq_record_t>::iterator i = potential_pairs.begin(); i != potential_pairs.end(); i++)
        *orphan_stream << "@" << i->first << endl << i->second.seq << endl << "+" << i->first << endl << i->second.qual << endl;

    if(open) {
        fwd_file.close();
        rev_file.close();
        orphan_file.close();
    }

    open = false;
    
    return true;
}

void compliment(string & str)
{
    for(int ctr = 0; ctr < str.size(); ctr++) {
        char & c = str[ctr];
        switch(c) {
            case 'A': c = 'T'; break;
            case 'C': c = 'G'; break;
            case 'G': c = 'C'; break;
            case 'T': c = 'A'; break;
            case 'a': c = 't'; break;
            case 'c': c = 'g'; break;
            case 'g': c = 'c'; break;
            case 't': c = 'a'; break;
        }
    }
}

bool FastqWriter::SaveAlignment(BamTools::BamAlignment & a) {
    a.BuildCharData();
    
    if(fwd_stream == rev_stream)
        *fwd_stream << "@" << a.Name << endl << a.QueryBases << endl << "+" << a.Name << endl << a.Qualities << endl;
    else {
        map<string, fastq_record_t>::iterator it = potential_pairs.find(a.Name);

        if(it == potential_pairs.end()) {
            fastq_record_t rec;
            rec.seq = a.QueryBases;
            rec.qual = a.Qualities;
            potential_pairs[a.Name] = rec;
        } else {
            fastq_record_t & rec = it->second;
            string seq = a.QueryBases;
            string qual = a.Qualities;

            string & fwd_seq = a.IsReverseStrand() ? rec.seq : seq;
            string & fwd_qual = a.IsReverseStrand() ? rec.qual : qual; 
            string & rev_seq = a.IsReverseStrand() ? seq : rec.seq;
            string & rev_qual = a.IsReverseStrand() ? qual : rec.qual; 

            reverse(rev_seq.begin(), rev_seq.end());
            reverse(rev_qual.begin(), rev_qual.end());
            compliment(rev_seq);

            *fwd_stream << "@" << a.Name << "/1" << endl << fwd_seq << endl << "+" << a.Name << "/1" << endl << fwd_qual << endl;
            *rev_stream << "@" << a.Name << "/2" << endl << rev_seq << endl << "+" << a.Name << "/2" << endl << rev_qual << endl;
            
            if(fwd_stream->fail() || rev_stream->fail()) {
                cerr << "Error writing out line to fastq file. Aborting." << endl;
                exit(-1);
            }

            potential_pairs.erase(it);
        }
    }
    
    return true;
}