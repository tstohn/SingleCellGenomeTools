#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>

#include "BarcodeProcessingHandler.hpp"
using namespace boost::program_options;

/**
 * HOW TO:
 * PREREQUISITS:
 *  THE BARCODE FILE TO ANALYSE MUST HAVE A HEADER WITH THE BARCODEPATTERNS AS IN FASTQPARSER TOOL, THERE MUST BE A UMI SEQU< AND A AB SEQ, which MUST BE THE ONLY
 *  VARYING BARCODE SEQUENCE EXCEPT THE CI BARCODES
 * 
 * - generates two files: 
 *          - firstly collapse all UMI_barcodes and have a file: UMI_idx, Ab_idx, cell_idx
 *          - secondly collapse all UMI_idxs: Ab_idx, Ab_count, cell_idx
 * 
 *  - get all unique UMIs
 *  - cluster UMIs that r similar to the same group of UMIs
 * => make a file where UMI column is the 'expected' UMI, from this file we can calculate the saturation...
 * 
 *  - delete doublicate reads (based on UMI and BC 1-4)
 *  - for the same cell (based on BC 1-4) count the number of UMIs per AB_barcode and write a new column AB_count,
 *    so that we end up with a file Ab_id, sample_id, Ab_count
 * => generate the file format for our ScRNA_seq_Normalization pipeline
 * 
 * DISTINGUISH THOSE TWO BY UMI-DATA and BARCODE-DATA
 * 
 * PARAMETER FOR ABRCODE POSITIONS:
 *  give a list of indices for the CIBarcodes within the file, which hold all alternatives for the varying indices (the real position indide the fastqRead is then parsed
 *  by checking for NNNN sequences), the other one left NNN sequence is the AB sequence and the XXX is the UMI sequence
 * */

bool parse_arguments(char** argv, int argc, std::string& inFile,  std::string& outFile, int& threats, 
                     std::string& barcodeFile, std::string& barcodeIndices, int& umiMismatches,
                     std::string& abFile, int& abIdx, std::string& treatmentFile, int& treatmentIdx,
                     std::string& classSeqFile, std::string& classNameFile)
{
    try
    {
        options_description desc("Options");
        desc.add_options()
            ("input,i", value<std::string>(&inFile)->required(), "input file of demultiplexed reads for ABs in Single cells in tsv format")
            ("output,o", value<std::string>(&outFile)->required(), "output file with all split barcodes")

            ("barcodeList,b", value<std::string>(&(barcodeFile)), "file with a list of all allowed well barcodes (comma seperated barcodes across several rows)\
            the row refers to the correponding bracket enclosed sequence substring. E.g. for two bracket enclosed substrings in out sequence a possible list could be:\
            AGCTTCGAG,ACGTTCAGG\nACGTCTAGACT,ATCGGCATACG,ATCGCGATC,ATCGCGCATAC. This can be the same list as it was for FastqParser. Do not inlcude the barcodes for the\
            guide reads here; add them as two seperate files guide reads are present.")
            ("antibodyList,a", value<std::string>(&(abFile)), "file with a list of all antbodies used, should be in same order as the ab-barcodes in the barcodeList.")
            ("antibodyIndex,x", value<int>(&abIdx), "Index used for antibody distinction.")
            ("groupList,g", value<std::string>(&(treatmentFile)), "file with a list of all groups (e.g.treatments) used, should be in same order as the specific arcodes in the barcodeList. \
            If tis argument is given, you must also add the index of barcodes used for grouping")
            ("GroupingIndex,y", value<int>(&treatmentIdx), "Index used to group cells(e.g. by treatment). This is the x-th barcode from the barcodeFile (0 indexed).")

            ("classSeq,s", value<std::string>(&(classSeqFile)), "file with the sequences that define origin of cells (e.g. sgRNA seauences along the experiment)")
            ("className,n", value<std::string>(&(classNameFile)), "file with names to replace the seuqence of origin")

            ("CombinatorialIndexingBarcodeIndices,c", value<std::string>(&(barcodeIndices))->required(), "comma seperated list of indexes, that are used during \
            combinatorial indexing and should distinguish a unique cell. Be aware that this is the index of the line inside the barcodeList file (see above). \
            This file ONLY includes lines for the varying sequences (except UMI). Therefore the index is not the same as the position in the whole sequence \
            if constant or UMI-seq are present. Index starts with zero.")

            ("mismatches,u", value<int>(&umiMismatches)->default_value(2), "number of allowed mismatches in a UMI. The nucleotides in the beginning and end do NOT count.\
            Since the UMI is defined as the sequence between the last and first match of neighboring sequences, bases of mismatches could be in the beginning/ end.")
            ("thread,t", value<int>(&threats)->default_value(5), "number of threads")
            ("help,h", "help message");

        variables_map vm;
        store(parse_command_line(argc, argv, desc), vm);

        if(vm.count("help"))
        {
            std::cout << desc << "\n";
            std::cout << "EXAMPLE CALL:\n ./bin/processing -i <inFile> ... \n";
            return false;
        }

        notify(vm);
    }
    catch(std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return false;
    }
    return true;
}

// generate a dictionary to map sequences to AB(proteins)
std::unordered_map<std::string, std::string > generateProteinDict(std::string abFile, int abIdx, 
                                                                                              const std::vector<std::string>& abBarcodes)
{
    std::unordered_map<std::string, std::string > map;
    std::vector<std::string> proteinNames;

    std::ifstream abFileStream(abFile);
    for(std::string line; std::getline(abFileStream, line);)
    {
        std::string delimiter = ",";
        std::string seq;
        size_t pos = 0;
        std::vector<std::string> seqVector;
        while ((pos = line.find(delimiter)) != std::string::npos) 
        {
            seq = line.substr(0, pos);
            line.erase(0, pos + 1);
            proteinNames.push_back(seq);
        }
        seq = line;
        proteinNames.push_back(seq);
    }

    assert(abBarcodes.size() == proteinNames.size());
    for(int i = 0; i < abBarcodes.size(); ++i)
    {
        map.insert(std::make_pair(abBarcodes.at(i), proteinNames.at(i)));
    }
    abFileStream.close();

    return map;
}

// generate a dictionary to map sequences to treatments
std::unordered_map<std::string, std::string > generateTreatmentDict(std::string treatmentFile, int treatmentIdx,
                                                                    const std::vector<std::string>& treatmentBarcodes)
{
    std::unordered_map<std::string, std::string > map;
    std::vector<std::string> treatmentNames;

    std::ifstream treatmentFileStream(treatmentFile);
    for(std::string line; std::getline(treatmentFileStream, line);)
    {
        std::string delimiter = ",";
        std::string seq;
        size_t pos = 0;
        std::vector<std::string> seqVector;
        while ((pos = line.find(delimiter)) != std::string::npos) 
        {
            seq = line.substr(0, pos);
            line.erase(0, pos + 1);
            treatmentNames.push_back(seq);

        }
        seq = line;
        treatmentNames.push_back(seq);
    }
    assert(treatmentNames.size() == treatmentBarcodes.size());
    for(int i = 0; i < treatmentBarcodes.size(); ++i)
    {
        map.insert(std::make_pair(treatmentBarcodes.at(i), treatmentNames.at(i)));
    }
    treatmentFileStream.close();

    return map;
}

// generate a dictionary to map sequences to treatments
std::unordered_map<std::string, std::string > generateClassDict(const std::string& classSeqFile,
                                                                const std::string& classNameFile)
{
    std::unordered_map<std::string, std::string > map;
    std::vector<std::string> names;
    std::vector<std::string> seqs;

    //parse all sequences
    std::ifstream seqFileStream(classSeqFile);
    for(std::string line; std::getline(seqFileStream, line);)
    {
        std::string delimiter = ",";
        std::string seq;
        size_t pos = 0;
        std::vector<std::string> seqVector;
        while ((pos = line.find(delimiter)) != std::string::npos) 
        {
            seq = line.substr(0, pos);
            line.erase(0, pos + 1);
            seqs.push_back(seq);
        }
        seq = line;
        seqs.push_back(seq);
    }
    seqFileStream.close();
    if(seqs.empty())
    {
        std::cout << "ERROR: Could not parse any sequence for guides! Check the guide sequence file.\n";
        exit(EXIT_FAILURE);
    }

    //parse all names
    std::ifstream nameFileStream(classNameFile);
    for(std::string line; std::getline(nameFileStream, line);)
    {
        std::string delimiter = ",";
        std::string seq;
        size_t pos = 0;
        std::vector<std::string> seqVector;
        while ((pos = line.find(delimiter)) != std::string::npos) 
        {
            seq = line.substr(0, pos);
            line.erase(0, pos + 1);
            names.push_back(seq);
        }
        seq = line;
        names.push_back(seq);
    }
    nameFileStream.close();
    if(names.empty())
    {
        std::cout << "ERROR: Could not parse any names for guides! Check the guide name file.\n";
        exit(EXIT_FAILURE);
    }

    if(names.size() != seqs.size())
    {
        std::cout << "ERROR: The number of sequences and names for guide reads does not match. Check files for guide sequences and names.\n";
        exit(EXIT_FAILURE);
    }

    for(int i = 0; i < names.size(); ++i)
    {
        map.insert(std::make_pair(seqs.at(i), names.at(i)));
    }

    return map;
}

int main(int argc, char** argv)
{

    std::string inFile;
    std::string outFile;
    std::string barcodeFile;
    std::string barcodeIndices;
    int thread;
    int umiMismatches;

    //data for protein(ab) and treatment information
    std::string abFile; 
    int abIdx;
    std::string treatmentFile;
    int treatmentIdx;
    std::vector<std::string> abBarcodes;
    std::vector<std::string> treatmentBarcodes;

    //data for class information (given e.g. by guide RNA)
    std::string classSeqFile;
    std::string classNameFile;

    parse_arguments(argv, argc, inFile, outFile, thread, barcodeFile, barcodeIndices, 
                    umiMismatches, abFile, abIdx, treatmentFile, treatmentIdx,
                    classSeqFile, classNameFile);
    
    //generate the dictionary of barcode alternatives to idx
    NBarcodeInformation barcodeIdData;
    generateBarcodeDicts(barcodeFile, barcodeIndices, barcodeIdData, abBarcodes, abIdx, &treatmentBarcodes, treatmentIdx);
    BarcodeProcessingHandler dataParser(barcodeIdData);

    //generate dictionaries to map sequences to the real names of Protein/ treatment/ etc...
    if(!abFile.empty())
    {
        std::unordered_map<std::string, std::string > map = generateProteinDict(abFile, abIdx, abBarcodes);
        dataParser.addProteinData(map);
    }
    if(!treatmentFile.empty())
    {
        std::unordered_map<std::string, std::string > map = generateTreatmentDict(treatmentFile, treatmentIdx, treatmentBarcodes);
        dataParser.addTreatmentData(map);
    }
    if(!classSeqFile.empty()) //could take any of both files: generateClassDict checks for both files beeing same length
    {
        std::unordered_map<std::string, std::string > map = generateClassDict(classSeqFile, classNameFile);
        dataParser.addClassData(map);
    }

    //parse the file of demultiplexed barcodes and
    //add all the data to the Unprocessed Demultiplexed Data (stored in rawData)
    // (AB, treatment already are mapped to their real names, scID is a concatenation of numbers for each barcode in
    //each abrcoding round, seperated by a dot)
    dataParser.parseFile(inFile, thread);
    //further process the data (correct UMIs, collapse same UMIs, etc.)
    dataParser.processBarcodeMapping(umiMismatches, thread);
    dataParser.writeLog(outFile);
    dataParser.writeAbCountsPerSc(outFile);

    return(EXIT_SUCCESS);
}