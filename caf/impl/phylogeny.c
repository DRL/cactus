/*
 * phlogeny.c
 *
 *  Created on: Jun 2, 2014
 *      Author: benedictpaten
 */

#include "sonLib.h"
#include "cactus.h"
#include "stPinchGraphs.h"
#include "stCactusGraphs.h"
#include "stPinchPhylogeny.h"
#include "stCaf.h"
#include "stCafPhylogeny.h"

stHash *stCaf_getThreadStrings(Flower *flower, stPinchThreadSet *threadSet) {
    stHash *threadStrings = stHash_construct2(NULL, free);
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Sequence *sequence = cap_getSequence(cap);
        assert(sequence != NULL);
        assert(stPinchThread_getLength(thread)-2 >= 0);
        char *string = sequence_getString(sequence, stPinchThread_getStart(thread)+1, stPinchThread_getLength(thread)-2, 1); //Gets the sequence excluding the empty positions representing the caps.
        char *paddedString = stString_print("N%sN", string); //Add in positions to represent the flanking bases
        stHash_insert(threadStrings, thread, paddedString);
        free(string);
    }
    return threadStrings;
}

stSet *stCaf_getOutgroupThreads(Flower *flower, stPinchThreadSet *threadSet) {
    stSet *outgroupThreads = stSet_construct();
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while ((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(event_isOutgroup(event)) {
            stSet_insert(outgroupThreads, thread);
        }
    }
    return outgroupThreads;
}


/*
 * Gets a list of the segments in the block that are part of outgroup threads.
 * The list contains stIntTuples, each of length 1, representing the index of a particular segment in
 * the block.
 */
static stList *getOutgroupThreads(stPinchBlock *block, stSet *outgroupThreads) {
    stList *outgroups = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        if(stSet_search(outgroupThreads, stPinchSegment_getThread(segment)) != NULL) {
            stList_append(outgroups, stIntTuple_construct1(i));
        }
        i++;
    }
    assert(i == stPinchBlock_getDegree(block));
    return outgroups;
}

/*
 * Splits the block using the given partition into a set of new blocks.
 */
void splitBlock(stPinchBlock *block, stList *partitions, bool allowSingleDegreeBlocks) {
    assert(stList_length(partitions) > 0);
    if(stList_length(partitions) == 1) {
        return; //Nothing to do.
    }
    //Build a mapping of indices of the segments in the block to the segments
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stPinchSegment **segments = st_calloc(blockDegree, sizeof(stPinchSegment *));
    bool *orientations = st_calloc(blockDegree, sizeof(bool));
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        segments[i] = segment;
        assert(segments[i] != NULL);
        orientations[i++] = stPinchSegment_getBlockOrientation(segment);
    }
    assert(i == stPinchBlock_getDegree(block));
    //Destruct old block, as we build new blocks now.
    stPinchBlock_destruct(block);
    //Now build the new blocks.
    for(int64_t i=0; i<stList_length(partitions); i++) {
        stList *partition = stList_get(partitions, i);
        assert(stList_length(partition) > 0);
        int64_t k = stIntTuple_get(stList_get(partition, 0), 0);
        assert(segments[k] != NULL);
        assert(stPinchSegment_getBlock(segments[k]) == NULL);

        if (!allowSingleDegreeBlocks && stList_length(partition) == 1) {
            // We need to avoid assigning this single-degree block
            segments[k] = NULL;
            continue;
        }

        block = stPinchBlock_construct3(segments[k], orientations[k]);
        assert(stPinchSegment_getBlock(segments[k]) == block);
        assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
        segments[k] = NULL; //Defensive, and used for debugging.
        for(int64_t j=1; j<stList_length(partition); j++) {
            k = stIntTuple_get(stList_get(partition, j), 0);
            assert(segments[k] != NULL);
            assert(stPinchSegment_getBlock(segments[k]) == NULL);
            stPinchBlock_pinch2(block, segments[k], orientations[k]);
            assert(stPinchSegment_getBlock(segments[k]) == block);
            assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
            segments[k] = NULL; //Defensive, and used for debugging.
        }
    }
    //Now check the segments have all been used - this is just debugging.
    for(int64_t i=0; i<blockDegree; i++) {
        assert(segments[i] == NULL);
    }
    //Cleanup
    free(segments);
    free(orientations);
}

/*
 * For logging purposes gets the total number of similarities and differences in the matrix.
 */
static void getTotalSimilarityAndDifferenceCounts(stMatrix *matrix, double *similarities, double *differences) {
    *similarities = 0.0;
    *differences = 0.0;
    for(int64_t i=0; i<stMatrix_n(matrix); i++) {
        for(int64_t j=i+1; j<stMatrix_n(matrix); j++) {
            *similarities += *stMatrix_getCell(matrix, i, j);
            *differences += *stMatrix_getCell(matrix, j, i);
        }
    }
}

// If the tree contains any zero branch lengths (i.e. there were
// negative branch lengths when neighbor-joining), fudge the branch
// lengths so that both children have non-zero branch lengths, but are
// still the same distance apart. When both children have zero branch
// lengths, give them both a small branch length. This makes
// likelihood methods usable.

// Only works on binary trees.
static void fudgeZeroBranchLengths(stTree *tree, double fudgeFactor, double smallNonZeroBranchLength) {
    assert(stTree_getChildNumber(tree) == 2 || stTree_getChildNumber(tree) == 0);
    assert(fudgeFactor < 1.0 && fudgeFactor > 0.0);
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        fudgeZeroBranchLengths(child, fudgeFactor, smallNonZeroBranchLength);
        if (stTree_getBranchLength(child) == 0.0) {
            stTree *otherChild = stTree_getChild(tree, !i);
            if (stTree_getBranchLength(otherChild) == 0.0) {
                // Both children have zero branch lengths, set them
                // both to some very small but non-zero branch length
                // so that probabilistic methods can actually work
                stTree_setBranchLength(child, smallNonZeroBranchLength);
                stTree_setBranchLength(otherChild, smallNonZeroBranchLength);
            } else {
                // Keep the distance between the children equal, but
                // move it by fudgeFactor so that no branch length is
                // zero.
                stTree_setBranchLength(child, fudgeFactor * stTree_getBranchLength(otherChild));
                stTree_setBranchLength(otherChild, (1 - fudgeFactor) * stTree_getBranchLength(otherChild));
            }
        }
    }
}

/*
 * Get a gene node->species node mapping from a gene tree, a species
 * tree, and the pinch block.
 */

static stHash *getLeafToSpecies(stTree *geneTree, stTree *speciesTree,
                                stPinchBlock *block, Flower *flower) {
    stHash *leafToSpecies = stHash_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        char *eventNameString = stString_print("%" PRIi64, event_getName(event));
        stTree *species = stTree_findChild(speciesTree, eventNameString);
        free(eventNameString);
        assert(species != NULL);
        stTree *gene = stPhylogeny_getLeafByIndex(geneTree, i);
        assert(gene != NULL);
        stHash_insert(leafToSpecies, gene, species);
        i++;
    }
    return leafToSpecies;
}

/*
 * Get a mapping from matrix index -> join cost index for use in
 * neighbor-joining guided by a species tree.
 */

static stHash *getMatrixIndexToJoinCostIndex(stPinchBlock *block, Flower *flower, stTree *speciesTree, stHash *speciesToJoinCostIndex) {
    stHash *matrixIndexToJoinCostIndex = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey, (int (*)(const void *, const void *)) stIntTuple_equalsFn, (void (*)(void *)) stIntTuple_destruct, (void (*)(void *)) stIntTuple_destruct);
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        char *eventNameString = stString_print("%" PRIi64, event_getName(event));
        stTree *species = stTree_findChild(speciesTree, eventNameString);
        free(eventNameString);
        assert(species != NULL);

        stIntTuple *joinCostIndex = stHash_search(speciesToJoinCostIndex, species);
        assert(joinCostIndex != NULL);

        stIntTuple *matrixIndex = stIntTuple_construct1(i);
        stHash_insert(matrixIndexToJoinCostIndex, matrixIndex,
                      // Copy the join cost index so it has the same
                      // lifetime as the hash
                      stIntTuple_construct1(stIntTuple_get(joinCostIndex, 0)));
        i++;
    }
    return matrixIndexToJoinCostIndex;
}

static stTree *eventTreeToStTree_R(Event *event) {
    stTree *ret = stTree_construct();
    stTree_setLabel(ret, stString_print("%" PRIi64, event_getName(event)));
    stTree_setBranchLength(ret, event_getBranchLength(event));
    for(int64_t i = 0; i < event_getChildNumber(event); i++) {
        Event *child = event_getChild(event, i);
        stTree *childStTree = eventTreeToStTree_R(child);
        stTree_setParent(childStTree, ret);
    }
    return ret;
}

// Get species tree from event tree (labeled by the event Names),
// which requires ignoring the root event.
static stTree *eventTreeToStTree(EventTree *eventTree) {
    Event *rootEvent = eventTree_getRootEvent(eventTree);
    // Need to skip the root event, since it is added onto the real
    // species tree.
    assert(event_getChildNumber(rootEvent) == 1);
    Event *speciesRoot = event_getChild(rootEvent, 0);
    return eventTreeToStTree_R(speciesRoot);
}

static double scoreTree(stTree *tree, enum stCaf_ScoringMethod scoringMethod, stTree *speciesStTree, stPinchBlock *block, Flower *flower, stList *featureColumns) {
    double ret = 0.0;
    if (scoringMethod == RECON_COST) {
        stHash *leafToSpecies = getLeafToSpecies(tree,
                                                 speciesStTree,
                                                 block, flower);
        int64_t dups, losses;
        stPhylogeny_reconciliationCostBinary(tree, speciesStTree,
                                                  leafToSpecies, &dups,
                                                  &losses);
        ret = -dups - losses;

        stHash_destruct(leafToSpecies);
    } else if (scoringMethod == NUCLEOTIDE_LIKELIHOOD) {
        ret = stPinchPhylogeny_likelihood(tree, featureColumns);
    } else if (scoringMethod == RECON_LIKELIHOOD) {
        // copy tree before use -- we are modifying the client-data
        // field. Not necessary if we end up adding
        // stReconciliationInfo before this method
        stTree *tmp = stTree_clone(tree);
        stHash *leafToSpecies = getLeafToSpecies(tmp,
                                                 speciesStTree,
                                                 block, flower);
        stPhylogeny_reconcileBinary(tmp, speciesStTree, leafToSpecies, false);
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tmp, speciesStTree, 1.0);
        stReconciliationInfo_destructOnTree(tmp);
        stTree_destruct(tmp);
    } else if (scoringMethod == COMBINED_LIKELIHOOD) {
        // copy tree before use -- we are modifying the client-data
        // field. Not necessary if we end up adding
        // stReconciliationInfo before this method
        stTree *tmp = stTree_clone(tree);
        stHash *leafToSpecies = getLeafToSpecies(tmp,
                                                 speciesStTree,
                                                 block, flower);
        stPhylogeny_reconcileBinary(tmp, speciesStTree, leafToSpecies, false);
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tmp, speciesStTree, 1.0);
        ret += stPinchPhylogeny_likelihood(tree, featureColumns);
        stReconciliationInfo_destructOnTree(tmp);
        stTree_destruct(tmp);        
    }
    return ret;
}

// Build a tree from a set of feature columns and root it according to
// the rooting method.
static stTree *buildTree(stList *featureColumns,
                         enum stCaf_TreeBuildingMethod treeBuildingMethod,
                         enum stCaf_RootingMethod rootingMethod,
                         double breakPointScalingFactor,
                         bool bootstrap,
                         stList *outgroups, stPinchBlock *block,
                         Flower *flower, stTree *speciesStTree,
                         stMatrix *joinCosts,
                         stHash *speciesToJoinCostIndex) {
    // Make substitution matrix
    stMatrix *substitutionMatrix = stPinchPhylogeny_getMatrixFromSubstitutions(featureColumns, block, NULL, bootstrap);
    assert(stMatrix_n(substitutionMatrix) == stPinchBlock_getDegree(block));
    assert(stMatrix_m(substitutionMatrix) == stPinchBlock_getDegree(block));
    //Make breakpoint matrix
    stMatrix *breakpointMatrix = stPinchPhylogeny_getMatrixFromBreakpoints(featureColumns, block, NULL, bootstrap);
    
    //Combine the matrices into distance matrices
    stMatrix_scale(breakpointMatrix, breakPointScalingFactor, 0.0);
    stMatrix *combinedMatrix = stMatrix_add(substitutionMatrix, breakpointMatrix);
    stMatrix *distanceMatrix = stPinchPhylogeny_getSymmetricDistanceMatrix(combinedMatrix);

    stTree *tree = NULL;
    if(rootingMethod == OUTGROUP_BRANCH) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, outgroups);
        } else {
            assert(treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-outgroup-branch rooting not supported with guided neighbor joining");
        }
    } else if(rootingMethod == LONGEST_BRANCH) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            assert(treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-branch rooting not supported with guided neighbor joining");
        }
    } else if(rootingMethod == BEST_RECON) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            // FIXME: Could move this out of the function as
            // well. It's the same for each tree generated for the
            // block.
            stHash *matrixIndexToJoinCostIndex = getMatrixIndexToJoinCostIndex(block, flower, speciesStTree,
                                                                               speciesToJoinCostIndex);
            tree = stPhylogeny_guidedNeighborJoining(combinedMatrix, joinCosts, matrixIndexToJoinCostIndex, speciesToJoinCostIndex, speciesStTree);
            stHash_destruct(matrixIndexToJoinCostIndex);
        }
        stHash *leafToSpecies = getLeafToSpecies(tree,
                                                 speciesStTree,
                                                 block, flower);
        stTree *newTree = stPhylogeny_rootAndReconcileBinary(tree, speciesStTree, leafToSpecies);
        stPhylogeny_addStPhylogenyInfo(newTree);

        stPhylogenyInfo_destructOnTree(tree);
        stTree_destruct(tree);
        stHash_destruct(leafToSpecies);
        tree = newTree;
    }

    // Needed for likelihood methods not to have 0/100% probabilities
    // overly often (normally, almost every other leaf has a branch
    // length of 0)
    fudgeZeroBranchLengths(tree, 0.02, 0.0001);

    return tree;
}

// Check if the block's phylogeny is simple:
// - the block has only one event, or
// - the block has < 3 segments, or
// - the block does not contain any segments that are part of an
//   outgroup thread.
static bool hasSimplePhylogeny(stPinchBlock *block,
                               stSet *outgroupThreads,
                               Flower *flower) {
    if(stPinchBlock_getDegree(block) <= 2) {
        return true;
    }
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    bool foundOutgroup = 0, found2Events = 0;
    Event *currentEvent = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        if(stSet_search(outgroupThreads, thread) != NULL) {
            foundOutgroup = 1;
        }
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(currentEvent == NULL) {
            currentEvent = event;
        } else if(currentEvent != event) {
            found2Events = 1;
        }
    }
    return !(foundOutgroup && found2Events);
}

// Check if the block contains as many species as segments
static bool isSingleCopyBlock(stPinchBlock *block, Flower *flower) {
    stSet *seenEvents = stSet_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(stSet_search(seenEvents, event) != NULL) {
            stSet_destruct(seenEvents);
            return false;
        }
        stSet_insert(seenEvents, event);
    }
    stSet_destruct(seenEvents);
    return true;
}

// relabel a tree so it's useful for debug output
static void relabelMatrixIndexedTree(stTree *tree, stHash *matrixIndexToName) {
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        relabelMatrixIndexedTree(stTree_getChild(tree, i), matrixIndexToName);
    }
    if (stTree_getChildNumber(tree) == 0) {
        stPhylogenyInfo *info = stTree_getClientData(tree);
        assert(info != NULL);
        assert(info->matrixIndex != -1);
        stIntTuple *query = stIntTuple_construct1(info->matrixIndex);
        char *header = stHash_search(matrixIndexToName, query);
        assert(header != NULL);
        stTree_setLabel(tree, stString_copy(header));
        stIntTuple_destruct(query);
    }
}

// Print the debug info for blocks that are normally not printed
// (those that cannot be partitioned). The debug info contains just
// the "partition", i.e. the sequences and positions within the block
// in a list of lists.
static void printSimpleBlockDebugInfo(Flower *flower, stPinchBlock *block, FILE *outFile) {
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    fprintf(outFile, "[[");
    int64_t i = 0;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
        Event *event = cap_getEvent(cap);
        const char *eventHeader = event_getHeader(event);
        char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
        
        if (i != 0) {
            fprintf(outFile, ",");
        }
        fprintf(outFile, "\"%s\"", segmentHeader);
        free(segmentHeader);
        i++;
    }
    assert(i == stPinchBlock_getDegree(block));
    fprintf(outFile, "]]\n");
}

// print debug info: "tree\tpartition\n" to the file
static void printTreeBuildingDebugInfo(Flower *flower, stPinchBlock *block, stTree *bestTree, stList *partition, stMatrix *matrix, double score, FILE *outFile) {
    // First get a map from matrix indices to names
    // The format we will use for leaf names is "genome.seq|posStart-posEnd"
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stHash *matrixIndexToName = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey,
                                                  (int (*)(const void *, const void *)) stIntTuple_equalsFn,
                                                  (void (*)(void *)) stIntTuple_destruct, free);
    int64_t i = 0;
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
        Event *event = cap_getEvent(cap);
        const char *eventHeader = event_getHeader(event);
        char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
        stHash_insert(matrixIndexToName, stIntTuple_construct1(i), segmentHeader);
        i++;
    }
    assert(i == blockDegree);

    // Relabel (our copy of) the best tree.
    stTree *treeCopy = stTree_clone(bestTree);
    relabelMatrixIndexedTree(treeCopy, matrixIndexToName);
    char *newick = stTree_getNewickTreeString(treeCopy);

    fprintf(outFile, "%s\t", newick);

    // Print the partition
    fprintf(outFile, "[");
    for (i = 0; i < stList_length(partition); i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        stList *subList = stList_get(partition, i);
        fprintf(outFile, "[");
        for (int64_t j = 0; j < stList_length(subList); j++) {
            if (j != 0) {
                fprintf(outFile, ",");
            }
            stIntTuple *index = stList_get(subList, j);
            assert(stIntTuple_get(index, 0) < blockDegree);
            char *header = stHash_search(matrixIndexToName, index);
            assert(header != NULL);
            fprintf(outFile, "\"%s\"", header);
        }
        fprintf(outFile, "]");
    }
    fprintf(outFile, "]\t");

    // print the matrix
    fprintf(outFile, "[");
    for (i = 0; i < stMatrix_m(matrix); i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        fprintf(outFile, "[");
        for (int64_t j = 0; j < stMatrix_n(matrix); j++) {
            if (j != 0) {
                fprintf(outFile, ",");
            }
            fprintf(outFile, "%lf", *stMatrix_getCell(matrix, i, j));
        }
        fprintf(outFile, "]");
    }
    fprintf(outFile, "]\t");

    // print the sequences corresponding to the matrix indices
    fprintf(outFile, "[");
    for (int64_t i = 0; i < blockDegree; i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        stIntTuple *query = stIntTuple_construct1(i);
        char *header = stHash_search(matrixIndexToName, query);
        assert(header != NULL);
        fprintf(outFile, "\"%s\"", header);
        stIntTuple_destruct(query);
    }
    fprintf(outFile, "]\t");

    // print the score
    fprintf(outFile, "%lf\n", score);
    stTree_destruct(treeCopy);
    free(newick);
    stHash_destruct(matrixIndexToName);
}

static int64_t countBasesBetweenSingleDegreeBlocks(stPinchThreadSet *threadSet) {
    stPinchThreadSetIt pinchThreadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    int64_t numBases = 0;
    int64_t numBasesInSingleCopyBlocks = 0;
    while ((thread = stPinchThreadSetIt_getNext(&pinchThreadIt)) != NULL) {
        stPinchSegment *segment = stPinchThread_getFirst(thread);
        if (segment == NULL) {
            // No segments on this thread.
            continue;
        }
        bool wasInSingleDegreeBlock = stPinchBlock_getDegree(stPinchSegment_getBlock(segment)) == 1;
        stPinchSegment *oldSegment = NULL;
        while ((segment = stPinchSegment_get3Prime(segment)) != NULL) {
            stPinchBlock *block = stPinchSegment_getBlock(segment);
            if (block == NULL) {
                // Segment without a block.
                continue;
            }
            bool isInSingleDegreeBlock = stPinchBlock_getDegree(block) == 1;
            if (isInSingleDegreeBlock) {
                numBasesInSingleCopyBlocks += stPinchBlock_getLength(block);
            }
            int64_t numBasesBetweenSegments = 0;
            if (oldSegment != NULL) {
                numBasesBetweenSegments = stPinchSegment_getStart(segment) - (stPinchSegment_getStart(oldSegment) + stPinchSegment_getLength(oldSegment));
            }
            assert(numBasesBetweenSegments >= 0); // could be 0 if the
                                                  // blocks aren't
                                                  // identical
            if (wasInSingleDegreeBlock && isInSingleDegreeBlock) {
                numBases += numBasesBetweenSegments;
            }
            oldSegment = segment;
            wasInSingleDegreeBlock = isInSingleDegreeBlock;
        }
    }
    // FIXME: tmp
    fprintf(stdout, "There were %" PRIi64 " bases in single degree blocks.\n", numBasesInSingleCopyBlocks);
    return numBases;
}

void stCaf_buildTreesToRemoveAncientHomologies(stPinchThreadSet *threadSet, stHash *threadStrings, stSet *outgroupThreads, Flower *flower, int64_t maxBaseDistance, int64_t maxBlockDistance, int64_t numTrees, enum stCaf_TreeBuildingMethod treeBuildingMethod, enum stCaf_RootingMethod rootingMethod, enum stCaf_ScoringMethod scoringMethod, double breakPointScalingFactor, bool skipSingleCopyBlocks, bool allowSingleDegreeBlocks, double costPerDupPerBase, double costPerLossPerBase, FILE *debugFile) {
    stPinchThreadSetBlockIt blockIt = stPinchThreadSet_getBlockIt(threadSet);
    stPinchBlock *block;

    //Hash in which we store a map of blocks to the partitions
    stHash *blocksToPartitions = stHash_construct2(NULL, NULL);

    //Get species tree as an stTree
    EventTree *eventTree = flower_getEventTree(flower);
    stTree *speciesStTree = eventTreeToStTree(eventTree);

    // Get info for guided neighbor-joining
    stHash *speciesToJoinCostIndex = stHash_construct2(NULL, (void (*)(void *)) stIntTuple_destruct);
    stMatrix *joinCosts = stPhylogeny_computeJoinCosts(speciesStTree, speciesToJoinCostIndex, costPerDupPerBase * 2 * maxBaseDistance, costPerLossPerBase * 2 * maxBaseDistance);

    //Count of the total number of blocks partitioned by an ancient homology
    int64_t totalBlocksSplit = 0;
    int64_t totalSingleCopyBlocksSplit = 0;
    double totalSubstitutionSimilarities = 0.0;
    double totalSubstitutionDifferences = 0.0;
    double totalBreakpointSimilarities = 0.0;
    double totalBreakpointDifferences = 0.0;
    double totalBestTreeScore = 0.0;
    double totalTreeScore = 0.0;
    int64_t sampledTreeWasBetterCount = 0;
    int64_t totalOutgroupThreads = 0;
    int64_t totalSimpleBlocks = 0;
    int64_t totalSingleCopyBlocks = 0;

    //The loop to build a tree for each block
    while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
        if(!hasSimplePhylogeny(block, outgroupThreads, flower)) { //No point trying to build a phylogeny for certain blocks.
            bool singleCopy = 0;
            if(isSingleCopyBlock(block, flower)) {
                singleCopy = 1;
                if(skipSingleCopyBlocks) {
                    continue;
                }
                totalSingleCopyBlocks++;
            }
            //Parameters.
            bool ignoreUnalignedBases = 1;
            bool onlyIncludeCompleteFeatureBlocks = 0;

            //Get the feature blocks
            stList *featureBlocks = stFeatureBlock_getContextualFeatureBlocks(block, maxBaseDistance, maxBlockDistance,
                    ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, threadStrings);

            //Make feature columns
            stList *featureColumns = stFeatureColumn_getFeatureColumns(featureBlocks, block);

            //Make substitution matrix
            stMatrix *substitutionMatrix = stPinchPhylogeny_getMatrixFromSubstitutions(featureColumns, block, NULL, 0);
            assert(stMatrix_n(substitutionMatrix) == stPinchBlock_getDegree(block));
            assert(stMatrix_m(substitutionMatrix) == stPinchBlock_getDegree(block));
            double similarities, differences;
            getTotalSimilarityAndDifferenceCounts(substitutionMatrix, &similarities, &differences);
            totalSubstitutionSimilarities += similarities;
            totalSubstitutionDifferences += differences;

            //Make breakpoint matrix
            stMatrix *breakpointMatrix = stPinchPhylogeny_getMatrixFromBreakpoints(featureColumns, block, NULL, 0);
            getTotalSimilarityAndDifferenceCounts(breakpointMatrix, &similarities, &differences);
            totalBreakpointSimilarities += similarities;
            totalBreakpointDifferences += differences;

            //Combine the matrices into distance matrices
            stMatrix_scale(breakpointMatrix, breakPointScalingFactor, 0.0);
            stMatrix *combinedMatrix = stMatrix_add(substitutionMatrix, breakpointMatrix);
            stMatrix *distanceMatrix = stPinchPhylogeny_getSymmetricDistanceMatrix(combinedMatrix);

            //Get the outgroup threads
            stList *outgroups = getOutgroupThreads(block, outgroupThreads);
            totalOutgroupThreads += stList_length(outgroups);

            //Build the canonical tree.
            stTree *blockTree = buildTree(featureColumns, GUIDED_NEIGHBOR_JOINING, rootingMethod,
                                          breakPointScalingFactor,
                                          0, outgroups, block, flower,
                                          speciesStTree, joinCosts, speciesToJoinCostIndex);

            // Sample the rest of the trees.
            stList *trees = stList_construct();
            stList_append(trees, blockTree);
            for(int64_t i = 0; i < numTrees - 1; i++) {
                stTree *tree = buildTree(featureColumns, GUIDED_NEIGHBOR_JOINING, rootingMethod,
                                         breakPointScalingFactor,
                                         1, outgroups, block, flower,
                                         speciesStTree, joinCosts, speciesToJoinCostIndex);
                stList_append(trees, tree);
            }

            // Get the best-scoring tree.
            double maxScore = -INFINITY;
            stTree *bestTree = NULL;
            bool sampledTreeWasBetter = false;
            for(int64_t i = 0; i < stList_length(trees); i++) {
                stTree *tree = stList_get(trees, i);
                double score = scoreTree(tree, scoringMethod,
                                         speciesStTree, block, flower,
                                         featureColumns);
                if(score != -INFINITY) { // avoid counting impossible
                                         // trees
                    totalTreeScore += score;
                }
                if(score > maxScore) {
                    sampledTreeWasBetterCount += (i >= 1 && !sampledTreeWasBetter) ? 1 : 0;
                    if (i != 0) {
                        sampledTreeWasBetter = true;
                    }
                    maxScore = score;
                    bestTree = tree;
                }
            }

            if(bestTree == NULL) {
                // Can happen if/when the nucleotide likelihood score
                // is used and a block is all N's. Just use the
                // canonical NJ tree in that case.
                bestTree = blockTree;
            }

            assert(bestTree != NULL);

            totalBestTreeScore += maxScore;
            //Get the partitions
            stList *partition = stPinchPhylogeny_splitTreeOnOutgroups(bestTree, outgroups);
            if(stList_length(partition) > 1) {
                if(singleCopy) {
                    totalSingleCopyBlocksSplit++;
                }
                totalBlocksSplit++;
            }
            stHash_insert(blocksToPartitions, block, partition);

            // Print debug info: block, best tree, partition for this block
            if (debugFile != NULL) {
                printTreeBuildingDebugInfo(flower, block, bestTree, partition, distanceMatrix, maxScore, debugFile);
            }

            //Cleanup
            stMatrix_destruct(substitutionMatrix);
            stMatrix_destruct(breakpointMatrix);
            for(int64_t i = 0; i < stList_length(trees); i++) {
                stTree *tree = stList_get(trees, i);
                stPhylogenyInfo_destructOnTree(tree);
                stTree_destruct(tree);
            }
            stList_destruct(featureColumns);
            stList_destruct(featureBlocks);
            stList_destruct(outgroups);
        } else {
            // Print debug info even for simple blocks.
            if (debugFile != NULL) {
                printSimpleBlockDebugInfo(flower, block, debugFile);
            }
            totalSimpleBlocks++;
        }
    }
    // Block count including skipped blocks.
    int64_t totalBlockCount = stPinchThreadSet_getTotalBlockNumber(threadSet);
    // Number of blocks that were actually considered while partitioning.
    int64_t blockCount = totalBlockCount - totalSimpleBlocks;
    fprintf(stdout, "Using phylogeny building, of %" PRIi64 " blocks considered (%" PRIi64 " total), %" PRIi64 " blocks were partitioned\n", blockCount, totalBlockCount, totalBlocksSplit);
    fprintf(stdout, "There were %" PRIi64 " outgroup threads seen total over all blocks\n", totalOutgroupThreads);
    fprintf(stdout, "In phylogeny building there were %f avg. substitution similarities %f avg. substitution differences\n", totalSubstitutionSimilarities/blockCount, totalSubstitutionDifferences/blockCount);
    fprintf(stdout, "In phylogeny building there were %f avg. breakpoint similarities %f avg. breakpoint differences\n", totalBreakpointSimilarities/blockCount, totalBreakpointDifferences/blockCount);
    fprintf(stdout, "In phylogeny building we saw an average score of %f for the best tree in each block, an average score of %f overall, and %" PRIi64 " trees total.\n", totalBestTreeScore/blockCount, totalTreeScore/(numTrees*blockCount), numTrees*blockCount);
    fprintf(stdout, "In phylogeny building we used a sampled tree instead of the canonical tree %" PRIi64 " times.\n", sampledTreeWasBetterCount);
    fprintf(stdout, "In phylogeny building we skipped %" PRIi64 " simple blocks.\n", totalSimpleBlocks);
    fprintf(stdout, "In phylogeny building there were %" PRIi64 " single copy blocks considered, of which %" PRIi64 " were partitioned.\n", totalSingleCopyBlocks, totalSingleCopyBlocksSplit);

    st_logDebug("Got homology partition for each block\n");

    fprintf(stdout, "Before partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));

    //Now walk through the blocks and do the actual splits, must be done after the fact using the blocks
    //in the original hash, as we are now disrupting and changing the original graph.
    stHashIterator *blockIt2 = stHash_getIterator(blocksToPartitions);
    while ((block = stHash_getNext(blockIt2)) != NULL) {
        stList *partition = stHash_search(blocksToPartitions, block);
        assert(partition != NULL);
        splitBlock(block, partition, allowSingleDegreeBlocks);
        stList_destruct(partition);
    }
    stHash_destructIterator(blockIt2);

    st_logDebug("Finished partitioning the blocks\n");
    fprintf(stdout, "After partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));

    //Cleanup
    stHash_destruct(blocksToPartitions);
    stTree_destruct(speciesStTree);
}