// ***************************************************************************
// BamReader_p.cpp (c) 2009 Derek Barnett
// Marth Lab, Department of Biology, Boston College
// ---------------------------------------------------------------------------
// Last modified: 28 November 2011 (DB)
// ---------------------------------------------------------------------------
// Provides the basic functionality for reading BAM files
// ***************************************************************************

#include "api/BamConstants.h"
#include "api/BamReader.h"
#include "api/IBamIODevice.h"
#include "api/BamParallelismSettings.h"
#include "api/internal/bam/BamHeader_p.h"
#include "api/internal/bam/BamRandomAccessController_p.h"
#include "api/internal/bam/BamReader_p.h"
#include "api/internal/index/BamStandardIndex_p.h"
#include "api/internal/io/BamDeviceFactory_p.h"
#include "api/internal/utils/BamException_p.h"
using namespace BamTools;
using namespace BamTools::Internal;

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <ctime>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <vector>
using namespace std;


//prefetch thread

void * prefetch_start(void * reader_ptr)
{
#ifdef __linux__
    prctl(PR_SET_NAME,"bt_brprefetch",0,0,0);
#endif
    BamReaderPrivate * reader = (BamReaderPrivate *) reader_ptr;
    int count = 0;

    while(reader->do_prefetch)
    {
        if(!reader->do_prefetch)
          break;

        BamAlignment * al = new BamAlignment();
        reader->prefetch_tell_fail.push(reader->m_stream.Tell());
        if(!reader->LoadNextAlignmentInternal(*al)) {
            delete al;
            al = NULL;
        }
        reader->prefetch_alignments.push(al);
        if(!al)
            break;

        // If we build up a ton of data in the queue,
        // sleep this thread for a while to allow other threads to catch up
        // these numbers were chosen to ensure that the queue never runs out while we are sleeping.
        // 
        // Another issue that we address here is the problem of many threads simultaneously reading
        // from disk- when using slower seeking disks (ie, non-SSD), the system can grind to a 
        // halt as a result of tons of threads causing reads trying to fill up their queues constantly.
        // This will send the load average through the roof.
        if(count % 300 == 0)    //don't check often
        {
            double load;
            getloadavg(&load, 1);
            
            if(load > BamParallelismSettings::availableCores() / 2 && reader->prefetch_alignments.size() > 400) {
                 while(reader->prefetch_alignments.size() > 100) usleep(20000);
            } else if( reader->prefetch_alignments.size() > 20000) {
                while (reader->prefetch_alignments.size() > 5000) usleep(20000);
            }
        }
        count++;
    }

    return NULL;
}

// constructor
BamReaderPrivate::BamReaderPrivate(BamReader* parent)
    : m_alignmentsBeginOffset(0)
    , m_parent(parent)
    , do_prefetch(false)
{
    m_isBigEndian = BamTools::SystemIsBigEndian();
}

// destructor
BamReaderPrivate::~BamReaderPrivate(void) {
    Close();
}

// closes the BAM file
bool BamReaderPrivate::Close(void) {

    StopPrefetch();

    // clear BAM metadata
    m_references.clear();
    m_header.Clear();

    // clear filename
    m_filename.clear();

    // close random access controller
    m_randomAccessController.Close();

    // if stream is open, attempt close
    if ( IsOpen() ) {
        try {
            m_stream.Close();
        } catch ( BamException& e ) {
            const string streamError = e.what();
            const string message = string("encountered error closing BAM file: \n\t") + streamError;
            SetErrorString("BamReader::Close", message);
            return false;
        }
    }

    // return success
    return true;
}

// creates an index file of requested type on current BAM file
bool BamReaderPrivate::CreateIndex(const BamIndex::IndexType& type) {

    // skip if BAM file not open
    if ( !IsOpen() ) {
        SetErrorString("BamReader::CreateIndex", "cannot create index on unopened BAM file");
        return false;
    }

    // attempt to create index
    if ( m_randomAccessController.CreateIndex(this, type) )
        return true;
    else {
        const string bracError = m_randomAccessController.GetErrorString();
        const string message = string("could not create index: \n\t") + bracError;
        SetErrorString("BamReader::CreateIndex", message);
        return false;
    }
}

// return path & filename of current BAM file
const string BamReaderPrivate::Filename(void) const {
    return m_filename;
}

string BamReaderPrivate::GetErrorString(void) const {
    return m_errorString;
}

// return header data as std::string
string BamReaderPrivate::GetHeaderText(void) const {
    return m_header.ToString();
}

// return header data as SamHeader object
SamHeader BamReaderPrivate::GetSamHeader(void) const {
    return m_header.ToSamHeader();
}

// retrieves next available alignment core data (returns success/fail)
// ** DOES NOT populate any character data fields (read name, bases, qualities, tag data, filename)
//    these can be accessed, if necessary, from the supportData
// useful for operations requiring ONLY positional or other alignment-related information
bool BamReaderPrivate::GetNextAlignment(BamAlignment& alignment) {

    // skip if stream not opened
    if ( !m_stream.IsOpen() )
        return false;

    try {

        // skip if region is set but has no alignments
        if ( m_randomAccessController.HasRegion() &&
             !m_randomAccessController.RegionHasAlignments() )
        {
            return false;
        }

        // if can't read next alignment
        if ( !LoadNextAlignment(alignment) )
            return false;

        // check alignment's region-overlap state
        BamRandomAccessController::RegionState state = m_randomAccessController.AlignmentState(alignment);

        // if alignment starts after region, no need to keep reading
        if ( state == BamRandomAccessController::AfterRegion )
            return false;

        // read until overlap is found
        while ( state != BamRandomAccessController::OverlapsRegion ) {

            // if can't read next alignment
            if ( !LoadNextAlignment(alignment) )
                return false;

            // check alignment's region-overlap state
            state = m_randomAccessController.AlignmentState(alignment);

            // if alignment starts after region, no need to keep reading
            if ( state == BamRandomAccessController::AfterRegion )
                return false;
        }

        // if we get here, we found the next 'valid' alignment
        // (e.g. overlaps current region if one was set, simply the next alignment if not)
        return true;

    } catch ( BamException& e ) {
        const string streamError = e.what();
        const string message = string("encountered error reading BAM alignment: \n\t") + streamError;
        SetErrorString("BamReader::GetNextAlignment", message);
        return false;
    }
}

BamAlignment * BamReaderPrivate::GetNextAlignment() {
    
    // skip if stream not opened
    if ( !m_stream.IsOpen() )
        return NULL;
    
    try {
        
        // skip if region is set but has no alignments
        if ( m_randomAccessController.HasRegion() &&
            !m_randomAccessController.RegionHasAlignments() ) {
            return NULL;
        }
        
        // if can't read next alignment
        BamAlignment * alignment = LoadNextAlignment();
        if ( !alignment ) return NULL;
        
        // check alignment's region-overlap state
        BamRandomAccessController::RegionState state = m_randomAccessController.AlignmentState(*alignment);
        
        // if alignment starts after region, no need to keep reading
        if ( state == BamRandomAccessController::AfterRegion )
            return NULL;
        
        // read until overlap is found
        while ( state != BamRandomAccessController::OverlapsRegion ) {
            
            // if can't read next alignment
            alignment = LoadNextAlignment();
            if ( !alignment ) return NULL;
            
            // check alignment's region-overlap state
            state = m_randomAccessController.AlignmentState(*alignment);
            
            // if alignment starts after region, no need to keep reading
            if ( state == BamRandomAccessController::AfterRegion )
                return NULL;
        }
        
        // if we get here, we found the next 'valid' alignment
        // (e.g. overlaps current region if one was set, simply the next alignment if not)
        return alignment;
        
    } catch ( BamException& e ) {
        const string streamError = e.what();
        const string message = string("encountered error reading BAM alignment: \n\t") + streamError;
        SetErrorString("BamReader::GetNextAlignment", message);
        return NULL;
    }
}

int BamReaderPrivate::GetReferenceCount(void) const {
    return m_references.size();
}

const RefVector& BamReaderPrivate::GetReferenceData(void) const {
    return m_references;
}

// returns RefID for given RefName (returns References.size() if not found)
int BamReaderPrivate::GetReferenceID(const string& refName) const {

    // retrieve names from reference data
    vector<string> refNames;
    RefVector::const_iterator refIter = m_references.begin();
    RefVector::const_iterator refEnd  = m_references.end();
    for ( ; refIter != refEnd; ++refIter)
        refNames.push_back( (*refIter).RefName );

    // return 'index-of' refName (or -1 if not found)
    int index = distance(refNames.begin(), find(refNames.begin(), refNames.end(), refName));
    if ( index == (int)m_references.size() ) return -1;
    else return index;
}

bool BamReaderPrivate::HasIndex(void) const {
    return m_randomAccessController.HasIndex();
}

bool BamReaderPrivate::IsOpen(void) const {
    return m_stream.IsOpen();
}

// load BAM header data
void BamReaderPrivate::LoadHeaderData(void) {
    m_header.Load(&m_stream);
}

// populates BamAlignment with alignment data under file pointer, returns success/fail
bool BamReaderPrivate::LoadNextAlignment(BamAlignment& alignment) {
    if(do_prefetch) {
        bool retval = false;
        
        //wait for something to be prefetched
        while(prefetch_alignments.size() == 0) usleep(5000);
        
        BamAlignment * al = prefetch_alignments.pop();
        
        if(!al)
            retval = false;
        else {
            retval = true;
            alignment = *al;
        }
        
        delete al;
        
        return retval;
    }
    else
        return LoadNextAlignmentInternal(alignment);
}
BamAlignment * BamReaderPrivate::LoadNextAlignment() {
    if(do_prefetch) {        
        //wait for something to be prefetched
        while(prefetch_alignments.size() == 0) usleep(5000);
        
        BamAlignment * al = prefetch_alignments.pop();
        
        return al;
    } else {
        BamAlignment * al = new BamAlignment;
        bool ok = LoadNextAlignmentInternal(*al);
        return ok ? al : NULL;
    }
}

bool BamReaderPrivate::LoadNextAlignmentInternal(BamAlignment& alignment) {

    // read in the 'block length' value, make sure it's not zero
    char buffer[sizeof(uint32_t)];

    if(0 == m_stream.Read(buffer, sizeof(uint32_t)))    //if we read 0, this is the EOF.
        return false;
    BamAlignment::BamAlignmentSupportData SupportData;
    SupportData.BlockLength = BamTools::UnpackUnsignedInt(buffer);
    if ( m_isBigEndian ) BamTools::SwapEndian_32(SupportData.BlockLength);
    if ( SupportData.BlockLength == 0 )
    if ( SupportData.BlockLength == 0 )
        return false;

    // read in core alignment data, make sure the right size of data was read
    char x[Constants::BAM_CORE_SIZE];
    if ( m_stream.Read(x, Constants::BAM_CORE_SIZE) != Constants::BAM_CORE_SIZE ) {
        cerr << "Expected more bytes reading BAM core. Is this file truncated or corrupted?" << endl;
        return false;
    }

    // swap core endian-ness if necessary
    if ( m_isBigEndian ) {
        for ( unsigned int i = 0; i < Constants::BAM_CORE_SIZE; i+=sizeof(uint32_t) )
            BamTools::SwapEndian_32p(&x[i]);
    }

    // set BamAlignment 'core' and 'support' data
    alignment.setRefID(BamTools::UnpackSignedInt(&x[0]));
    alignment.setPosition(BamTools::UnpackSignedInt(&x[4]));

    unsigned int tempValue = BamTools::UnpackUnsignedInt(&x[8]);
    alignment.setBin(tempValue >> 16);
    alignment.setMapQuality(tempValue >> 8 & 0xff);
    SupportData.QueryNameLength = tempValue & 0xff;

    tempValue = BamTools::UnpackUnsignedInt(&x[12]);
    alignment.setAlignmentFlag(tempValue >> 16);
    SupportData.NumCigarOperations = tempValue & 0xffff;

    SupportData.QuerySequenceLength = BamTools::UnpackUnsignedInt(&x[16]);
    alignment.setMateRefID(BamTools::UnpackSignedInt(&x[20]));
    alignment.setMatePosition(BamTools::UnpackSignedInt(&x[24]));
    alignment.setInsertSize(BamTools::UnpackSignedInt(&x[28]));

    // read in character data - make sure proper data size was read
    bool readCharDataOK = false;
    const unsigned int dataLength = SupportData.BlockLength - Constants::BAM_CORE_SIZE;
    RaiiBuffer allCharData(dataLength);

    if ( m_stream.Read(allCharData.Buffer, dataLength) == dataLength ) {

        // store 'allCharData' in supportData structure
        SupportData.AllCharData.assign((const char*)allCharData.Buffer, dataLength);

        // set success flag
        readCharDataOK = true;

        // save CIGAR ops
        // need to calculate this here so that  BamAlignment::GetEndPosition() performs correctly,
        // even when GetNextAlignment() is called
        vector<CigarOp> CigarData;
        CigarData.reserve(SupportData.NumCigarOperations);
        const unsigned int cigarDataOffset = SupportData.QueryNameLength;
        uint32_t* cigarData = (uint32_t*)(allCharData.Buffer + cigarDataOffset);
        for ( unsigned int i = 0; i < SupportData.NumCigarOperations; ++i ) {

            // swap endian-ness if necessary
            if ( m_isBigEndian ) BamTools::SwapEndian_32(cigarData[i]);

            // build CigarOp structure
            CigarOp op;
            op.Length = (cigarData[i] >> Constants::BAM_CIGAR_SHIFT);
            op.Type   = Constants::BAM_CIGAR_LOOKUP[ (cigarData[i] & Constants::BAM_CIGAR_MASK) ];

            // save CigarOp
            CigarData.push_back(op);
        }
        
        alignment.setCigarData(CigarData);
    } else {
        cerr << "Expected more bytes reading BAM char data. Is this file truncated or corrupted?" << endl;
        return false;
    }
    
    SupportData.HasCoreOnly = true;
    alignment.setSupportData(SupportData);

    // return success/failure
    return readCharDataOK;
}

// loads reference data from BAM file
bool BamReaderPrivate::LoadReferenceData(void) {

    // get number of reference sequences
    char buffer[sizeof(uint32_t)];
    m_stream.Read(buffer, sizeof(uint32_t));
    uint32_t numberRefSeqs = BamTools::UnpackUnsignedInt(buffer);
    if ( m_isBigEndian ) BamTools::SwapEndian_32(numberRefSeqs);
    m_references.reserve((int)numberRefSeqs);

    // iterate over all references in header
    for ( unsigned int i = 0; i != numberRefSeqs; ++i ) {

        // get length of reference name
        m_stream.Read(buffer, sizeof(uint32_t));
        uint32_t refNameLength = BamTools::UnpackUnsignedInt(buffer);
        if ( m_isBigEndian ) BamTools::SwapEndian_32(refNameLength);
        RaiiBuffer refName(refNameLength);

        // get reference name and reference sequence length
        m_stream.Read(refName.Buffer, refNameLength);
        m_stream.Read(buffer, sizeof(int32_t));
        int32_t refLength = BamTools::UnpackSignedInt(buffer);
        if ( m_isBigEndian ) BamTools::SwapEndian_32(refLength);

        // store data for reference
        RefData aReference;
        aReference.RefName   = (string)((const char*)refName.Buffer);
        aReference.RefLength = refLength;
        m_references.push_back(aReference);
    }

    // return success
    return true;
}

bool BamReaderPrivate::LocateIndex(const BamIndex::IndexType& preferredType) {

    if ( m_randomAccessController.LocateIndex(this, preferredType) )
        return true;
    else {
        const string bracError = m_randomAccessController.GetErrorString();
        const string message = string("could not locate index: \n\t") + bracError;
        SetErrorString("BamReader::LocateIndex", message);
        return false;
    }
}

// opens BAM file (and index)
bool BamReaderPrivate::Open(const string& filename) {

    try {

        // make sure we're starting with fresh state
        Close();

        // open BgzfStream
        m_stream.Open(filename, IBamIODevice::ReadOnly);

        // load BAM metadata
        LoadHeaderData();
        LoadReferenceData();

        // store filename & offset of first alignment
        m_filename = filename;
        m_alignmentsBeginOffset = m_stream.Tell();
      
        do_prefetch = BamParallelismSettings::isMultithreadingEnabled() ;
      
        if(do_prefetch && 0 != pthread_create(&prefetch_thread, NULL, prefetch_start, this)) {
          perror("Failed to create BamReader prefetch thread");
          do_prefetch = false;
        }

        // return success
        return true;

    } catch ( BamException& e ) {
        const string error = e.what();
        const string message = string("could not open file: ") + filename +
                               "\n\t" + error;
        SetErrorString("BamReader::Open", message);
        return false;
    }
}

bool BamReaderPrivate::OpenIndex(const std::string& indexFilename) {

    if ( m_randomAccessController.OpenIndex(indexFilename, this) )
        return true;
    else {
        const string bracError = m_randomAccessController.GetErrorString();
        const string message = string("could not open index: \n\t") + bracError;
        SetErrorString("BamReader::OpenIndex", message);
        return false;
    }
}

// returns BAM file pointer to beginning of alignment data
bool BamReaderPrivate::Rewind(void) {
  
    StopPrefetch();

    // reset region
    m_randomAccessController.ClearRegion();

    // return status of seeking back to first alignment
    if ( Seek(m_alignmentsBeginOffset) )
        return true;
    else {
        const string currentError = m_errorString;
        const string message = string("could not rewind: \n\t") + currentError;
        SetErrorString("BamReader::Rewind", message);
        return false;
    }
}

bool BamReaderPrivate::Seek(const int64_t& position) {

    // skip if BAM file not open
    if ( !IsOpen() ) {
        SetErrorString("BamReader::Seek", "cannot seek on unopened BAM file");
        return false;
    }

    StopPrefetch();

    try {
        m_stream.Seek(position);
        return true;
    }
    catch ( BamException& e ) {
        const string streamError = e.what();
        const string message = string("could not seek in BAM file: \n\t") + streamError;
        SetErrorString("BamReader::Seek", message);
        return false;
    }
}

void BamReaderPrivate::SetErrorString(const string& where, const string& what) {
    static const string SEPARATOR = ": ";
    m_errorString = where + SEPARATOR + what;
}

void BamReaderPrivate::SetIndex(BamIndex* index) {
    m_randomAccessController.SetIndex(index);
}

// sets current region & attempts to jump to it
// returns success/failure
bool BamReaderPrivate::SetRegion(const BamRegion& region) {

    if ( m_randomAccessController.SetRegion(region, m_references.size()) )
        return true;
    else {
        const string bracError = m_randomAccessController.GetErrorString();
        const string message = string("could not set region: \n\t") + bracError;
        SetErrorString("BamReader::SetRegion", message);
        return false;
    }
}

void BamReaderPrivate::StopPrefetch()
{
  if(!do_prefetch)
    return;

  do_prefetch = false;

  if(do_prefetch && 0 != pthread_join(prefetch_thread, NULL))
    perror("Error joining prefetch thread");
}

int64_t BamReaderPrivate::Tell(void) const {
    return m_stream.Tell();
}
