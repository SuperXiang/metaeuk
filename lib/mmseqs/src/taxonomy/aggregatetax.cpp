#include "NcbiTaxonomy.h"
#include "Parameters.h"
#include "DBWriter.h"
#include "FileUtil.h"
#include "Debug.h"
#include "Util.h"
#include "Matcher.h"
#include <map>
#include <algorithm>

#ifdef OPENMP
#include <omp.h>
#endif

const double MAX_WEIGHT = 1000;

struct taxHit {
    void setByEntry(const TaxID & taxonInput, const bool useAln, const char ** taxHitData, const size_t numCols, const int voteMode) {
        // plain format: 3+ tax columns: taxid, rank (can be more than one col), name (can be more than one col)
        // taxid + aln format has 11 columns: taxid, tkey, bitscore, seqid, evalue, qs, qe, ql, ts, te, tl
        taxon = taxonInput;
        evalue = 1.0;
        weight = 0.0;

        // if voteMode is evalue-based, all tax-assigned sequences should have alignment info...
        if ((taxon != 0) && (numCols < Matcher::ALN_RES_WITHOUT_BT_COL_CNT) && (useAln == true)) {
            Debug(Debug::ERROR) << "voteMode is evalue-based but taxonid: " << taxon << " does not have alignment info.\n";
            EXIT(EXIT_FAILURE);
        }

        // extract from alignment info
        if (useAln == true) {
            evalue = strtod(taxHitData[3],NULL);
        }

        // update weight according to mode
        if (voteMode == Parameters::AGG_TAX_UNIFORM) {
            weight = 1.0;
        } else if (voteMode == Parameters::AGG_TAX_MINUS_LOG_EVAL) {
            if (evalue > 0) {
                weight = -log(evalue);
            } else {
                weight = MAX_WEIGHT;
            }
        }
    }

    TaxID taxon;
    double evalue;
    double weight;
};

TaxID selectTaxForSet (const std::vector<taxHit> &setTaxa, NcbiTaxonomy const *taxonomy, const float majorityCutoff, 
                        size_t &numAssignedSeqs, size_t &numUnassignedSeqs, size_t &numSeqsAgreeWithSelectedTaxon, double &selectedPercent) {
    // count num occurences of each ancestor, possibly weighted 
    std::map<TaxID,double> ancTaxIdsCounts;
    
    // initialize counters and weights
    numAssignedSeqs = 0;
    numUnassignedSeqs = 0;
    numSeqsAgreeWithSelectedTaxon = 0;
    selectedPercent = 0;
    double totalAssignedSeqsWeights = 0.0;

    for (size_t i = 0; i < setTaxa.size(); ++i) {
        TaxID currTaxId = setTaxa[i].taxon;
        double currWeight = setTaxa[i].weight;
        // ignore unassigned sequences
        if (currTaxId == 0) {
            numUnassignedSeqs++;
            continue;
        }
        TaxonNode const * node = taxonomy->taxonNode(currTaxId, false);
        if (node == NULL) {
            Debug(Debug::ERROR) << "taxonid: " << currTaxId << " does not match a legal taxonomy node.\n";
            EXIT(EXIT_FAILURE);
        }
        totalAssignedSeqsWeights += currWeight;
        numAssignedSeqs++;

        // add currWeight
        if (ancTaxIdsCounts.find(currTaxId) != ancTaxIdsCounts.end()) {
            ancTaxIdsCounts[currTaxId] += currWeight;
        } else {
            ancTaxIdsCounts.insert(std::pair<TaxID,unsigned int>(currTaxId,currWeight));
        }

        // iterate all ancestors up to the root (including) and add the currWeight to each
        TaxID currParentTaxId = node->parentTaxId;
        while (currParentTaxId != currTaxId) {
            if (ancTaxIdsCounts.find(currParentTaxId) != ancTaxIdsCounts.end()) {
                ancTaxIdsCounts[currParentTaxId] += currWeight;
            } else {
                ancTaxIdsCounts.insert(std::pair<TaxID,unsigned int>(currParentTaxId,currWeight));
            }
            // move up:
            currTaxId = currParentTaxId;
            TaxonNode const * node = taxonomy->taxonNode(currParentTaxId, false);
            currParentTaxId = node->parentTaxId;
        }     
    }

    // select the lowest ancestor that meets the cutoff
    int minRank = INT_MAX;
    TaxID selctedTaxon = 0;

    for (std::map<TaxID,double>::iterator it = ancTaxIdsCounts.begin(); it != ancTaxIdsCounts.end(); it++) {
        double currPercent = float(it->second) / totalAssignedSeqsWeights;
        if (currPercent >= majorityCutoff) {
            TaxID currTaxId = it->first;
            TaxonNode const * node = taxonomy->taxonNode(currTaxId, false);
            int currRankInd = NcbiTaxonomy::findRankIndex(node->rank);
            if (currRankInd > 0) {
                if ((currRankInd < minRank) || ((currRankInd == minRank) && (currPercent > selectedPercent))) {
                    selctedTaxon = currTaxId;
                    minRank = currRankInd;
                    selectedPercent = currPercent;
                }
            }
        }
    }

    // count the number of seqs who have selectedTaxon in their ancestors (agree with selection):
    if (selctedTaxon == 1) {
        // all agree with "root"
        numSeqsAgreeWithSelectedTaxon = numAssignedSeqs;
        return (selctedTaxon);
    }
    if (selctedTaxon == 0) {
        // nothing informative
        return (selctedTaxon);
    }
    // otherwise, iterate over all seqs
    for (size_t i = 0; i < setTaxa.size(); ++i) {
        TaxID currTaxId = setTaxa[i].taxon;
        // ignore unassigned sequences
        if (currTaxId == 0) {
            continue;
        }
        TaxonNode const * node = taxonomy->taxonNode(currTaxId, false);

        // iterate all ancestors up to the root
        TaxID currParentTaxId = node->parentTaxId;
        while (currParentTaxId != currTaxId) {
            if (currTaxId == selctedTaxon) {
                numSeqsAgreeWithSelectedTaxon++;
                break;
            }
            currTaxId = currParentTaxId;
            TaxonNode const * node = taxonomy->taxonNode(currParentTaxId, false);
            currParentTaxId = node->parentTaxId;
        }
    }

    return (selctedTaxon);
}

int aggregate(const bool useAln, int argc, const char **argv, const Command& command) {
    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);

    // open taxonomy - evolutionary relationships amongst taxa
    NcbiTaxonomy * t = NcbiTaxonomy::openTaxonomy(par.db1);
    
    // open mapping of set to sequence
    DBReader<unsigned int> setToSeqReader(par.db2.c_str(), par.db2Index.c_str(), par.threads, DBReader<unsigned int>::USE_INDEX|DBReader<unsigned int>::USE_DATA);
    setToSeqReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    // open tax assignments per sequence
    DBReader<unsigned int> taxSeqReader(par.db3.c_str(), par.db3Index.c_str(), par.threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
    taxSeqReader.open(DBReader<unsigned int>::NOSORT);

    // open alignment per sequence - will be used only if useAln
    DBReader<unsigned int>* alnSeqReader = NULL;
    if (useAln == true) {
        alnSeqReader = new DBReader<unsigned int>(par.db4.c_str(), par.db4Index.c_str(), par.threads, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
        alnSeqReader->open(DBReader<unsigned int>::NOSORT);
    }

    // output is either db4 or db5
    std::string outDbStr = par.db4;
    std::string outDbIndexStr = par.db4Index;
    if (useAln == true) {
        outDbStr = par.db5;
        outDbIndexStr = par.db5Index;
    } else if (par.voteMode == Parameters::AGG_TAX_MINUS_LOG_EVAL) {
        Debug(Debug::ERROR) << "voteMode is evalue-based but no alignment databse was provided. consider calling aggregatetaxweights\n";
        EXIT(EXIT_FAILURE);
    }

    DBWriter writer(outDbStr.c_str(), outDbIndexStr.c_str(), par.threads, par.compressed, Parameters::DBTYPE_TAXONOMICAL_RESULT);
    writer.open();

    std::vector<std::string> ranks = NcbiTaxonomy::parseRanks(par.lcaRanks);

    Debug::Progress progress(taxSeqReader.getSize());

    #pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif
        // per thread variables
        const char *entry[2048];
        std::vector<taxHit> setTaxa;
        std::string setTaxStr;
        setTaxStr.reserve(4096);

        #pragma omp for schedule(dynamic, 10)
        for (size_t i = 0; i < setToSeqReader.getSize(); ++i) {
            progress.updateProgress();

            unsigned int setKey = setToSeqReader.getDbKey(i);

            char *results = setToSeqReader.getData(i, thread_idx);

            // process a specific set
            while (*results != '\0') {
                Util::getWordsOfLine(results, entry, 255);
                unsigned int seqKey = Util::fast_atoi<unsigned int>(entry[0]);

                char *seqToTaxData = taxSeqReader.getDataByDBKey(seqKey, thread_idx);
                Util::getWordsOfLine(seqToTaxData, entry, 255);
                TaxID taxon = Util::fast_atoi<int>(entry[0]);
                size_t numCols = 0;

                if (useAln == true) {
                    char *seqToAlnData = alnSeqReader->getDataByDBKey(seqKey, thread_idx);
                    numCols = Util::getWordsOfLine(seqToAlnData, entry, 255);
                }

                taxHit currTaxHit;
                currTaxHit.setByEntry(taxon, useAln, entry, numCols, par.voteMode);
                setTaxa.emplace_back(currTaxHit);
                results = Util::skipLine(results);
            }

            // aggregate - the counters will be filled by the section function:
            size_t numAssignedSeqs = 0;
            size_t numUnassignedSeqs = 0;
            size_t numSeqsAgreeWithSelectedTaxon = 0;
            double selectedPercent = 0;

            TaxID setSelectedTaxon = selectTaxForSet(setTaxa, t, par.majorityThr, numAssignedSeqs, numUnassignedSeqs, numSeqsAgreeWithSelectedTaxon, selectedPercent);
            TaxonNode const * node = t->taxonNode(setSelectedTaxon, false);

            size_t totalNumSeqs = numAssignedSeqs + numUnassignedSeqs;
            
            // prepare write
            if ((setSelectedTaxon == 0) || (node == NULL)) {
                setTaxStr.append(SSTR(0));
                setTaxStr.append(1, '\t');
                setTaxStr.append("no rank");
                setTaxStr.append(1, '\t');
                setTaxStr.append("unclassified");
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(totalNumSeqs));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(numAssignedSeqs));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(numSeqsAgreeWithSelectedTaxon));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(roundf(selectedPercent * 100) / 100));

                if (!ranks.empty()) {
                    setTaxStr += '\t';
                }
                if (par.showTaxLineage > 0) {
                    setTaxStr += '\t';
                }
            } else {
                setTaxStr.append(SSTR(node->taxId));
                setTaxStr.append(1, '\t');
                setTaxStr.append(node->rank);
                setTaxStr.append(1, '\t');
                setTaxStr.append(node->name);
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(totalNumSeqs));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(numAssignedSeqs));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(numSeqsAgreeWithSelectedTaxon));
                setTaxStr.append(1, '\t');
                setTaxStr.append(SSTR(roundf(selectedPercent * 100) / 100));

                if (!ranks.empty()) {
                    std::string lcaRanks = Util::implode(t->AtRanks(node, ranks), ';');
                    setTaxStr += '\t' + lcaRanks;
                }
                if (par.showTaxLineage == 1) {
                    setTaxStr += '\t' + t->taxLineage(node, true);
                }
                if (par.showTaxLineage == 2) {
                    setTaxStr += '\t' + t->taxLineage(node, false);
                }
            }
            setTaxStr += '\n';

            writer.writeData(setTaxStr.c_str(), setTaxStr.size(), setKey, thread_idx);
            setTaxStr.clear();

            // ready to move to the next set
            setTaxa.clear();
        }
    };
    Debug(Debug::INFO) << "\n";

    writer.close();
    taxSeqReader.close();
    setToSeqReader.close();
    alnSeqReader->close();
    delete alnSeqReader;
    delete t;

    return EXIT_SUCCESS;

}

int aggregatetaxweights(int argc, const char **argv, const Command& command) {
    return (aggregate(true, argc, argv, command)); 
}

int aggregatetax(int argc, const char **argv, const Command& command) {
    return (aggregate(false, argc, argv, command)); 
}