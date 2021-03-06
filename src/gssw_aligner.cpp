#include "gssw_aligner.hpp"
#include "json2pb.h"

// log(10)
static const double quality_scale_factor = 10.0 / log(10.0);
static const double exp_overflow_limit = log(std::numeric_limits<double>::max());

using namespace vg;
using namespace std;

double add_log(double log_x, double log_y) {
    if (log_x > log_y) {
        return log_x + log(1.0 + exp(log_y - log_x));
    }
    else {
        return log_y + log(1.0 + exp(log_x - log_y));
    }
}

Aligner::~Aligner(void) {
    free(nt_table);
    free(score_matrix);
}

Aligner::Aligner(int32_t _match,
                 int32_t _mismatch,
                 int32_t _gap_open,
                 int32_t _gap_extension)
{
    match = _match;
    mismatch = _mismatch;
    gap_open = _gap_open;
    gap_extension = _gap_extension;
    
    // these are used when setting up the nodes
    nt_table = gssw_create_nt_table();
    score_matrix = gssw_create_score_matrix(match, mismatch);
    log_base = 0.0;
}


gssw_graph* Aligner::create_gssw_graph(Graph& g, bool add_pinning_node, gssw_node** gssw_pinned_node_out) {
    
    // add a dummy sink node if we're pinning
    gssw_graph* graph = gssw_graph_create(g.node_size() + add_pinning_node);
    unordered_map<int64_t, gssw_node*> nodes;
    
    for (int i = 0; i < g.node_size(); ++i) {
        Node* n = g.mutable_node(i);
        // switch any non-ATGCN characters from the node sequence to N
        auto cleaned_seq = nonATGCNtoN(n->sequence());
        gssw_node* node = (gssw_node*)gssw_node_create(n, n->id(),
                                                       cleaned_seq.c_str(),
                                                       nt_table,
                                                       score_matrix);
        nodes[n->id()] = node;
        gssw_graph_add_node(graph, node);
    }
    
    unordered_set<int64_t> non_sink_nodes;

    for (int i = 0; i < g.edge_size(); ++i) {
        // Convert all the edges
        Edge* e = g.mutable_edge(i);
        if(!e->from_start() && !e->to_end()) {
            // This is a normal end to start edge.
            gssw_nodes_add_edge(nodes[e->from()], nodes[e->to()]);
            non_sink_nodes.insert(e->from());
        } else if(e->from_start() && e->to_end()) {
            // This is a start to end edge, but isn't reversing and can be converted to a normal end to start edge.

            // Flip the start and end
            gssw_nodes_add_edge(nodes[e->to()], nodes[e->from()]);
            non_sink_nodes.insert(e->to());
        } else {
            // TODO: It's a reversing edge, which gssw doesn't support yet. What
            // we should really do is do a topological sort to break cycles, and
            // then flip everything at the lower-rank end of this edge around,
            // so we don't have to deal with its reversing-ness. But for now we
            // just die so we don't get nonsense into gssw.
#pragma omp critical
            {
                // We need the critical section so we don't throw uncaught
                // exceptions in multiple threads at once, leading to C++ trying
                // to run termiante in parallel. This doesn't make it safe, just
                // slightly safer.
                cerr << "Can't gssw over reversing edge " <<e->from() << (e->from_start() ? " start" : " end") << " -> "
                     << e->to() << (e->to_end() ? " end" : " start")  << endl;
                // TODO: there's no safe way to kill the program without a way
                // to signal the master to do it, via a shared variable in the
                // clause that made us parallel.
            }
            exit(1);
        }
    }
    
    if (add_pinning_node) {
        // create a dummy sink node for pinning
        gssw_node* pinned_node = gssw_node_create(nullptr, numeric_limits<uint32_t>::max(),
                                                  "N", nt_table, score_matrix);
        
        nodes[numeric_limits<uint32_t>::max()] = pinned_node;
        gssw_graph_add_node(graph, pinned_node);
        
        *gssw_pinned_node_out = pinned_node;
        

        
        // connect all sink nodes to the dummy node
        for (int i = 0; i < g.node_size(); i++) {
            if (!non_sink_nodes.count(g.node(i).id())) {
                gssw_nodes_add_edge(nodes[g.node(i).id()], nodes[numeric_limits<uint32_t>::max()]);
            }
        }
    }
    
    return graph;

}

void Aligner::align_internal(Alignment& alignment, vector<Alignment>* multi_alignments, Graph& g,
                             bool pinned, bool pin_left, int32_t max_alt_alns, int8_t full_length_bonus,
                             bool print_score_matrices) {

    // check input integrity
    if (pin_left && !pinned) {
        cerr << "error:[Aligner] cannot choose pinned end in non-pinned alignment" << endl;
        exit(EXIT_FAILURE);
    }
    if (multi_alignments && !pinned) {
        cerr << "error:[Aligner] multiple traceback is not implemented in local alignment, only pinned and global" << endl;
        exit(EXIT_FAILURE);
    }
    if (!multi_alignments && max_alt_alns != 1) {
        cerr << "error:[Aligner] cannot specify maximum number of alignments in single alignment" << endl;
        exit(EXIT_FAILURE);
    }
    
    
    // alignment pinning algorithm is based on pinning in bottom right corner, if pinning in top
    // left we need to reverse all the sequences first and translate the alignment back later
    
    // create reversed graph if necessary
    Graph reversed_graph;
    if (pin_left) {
        reverse_graph(g, reversed_graph);
    }

    // choose forward or reversed objects
    // note: have to make a copy of the sequence because we will modify it to add a pinning point
    Graph* align_graph = &g;
    string align_sequence = alignment.sequence();
    if (pinned) {
        if (pin_left) {
            align_graph = &reversed_graph;
            reverse(align_sequence.begin(), align_sequence.end());
        }
        
        // add dummy ending to align to dummy pinned node
        align_sequence += "N";
    }
    
    // convert into gssw graph and get dummy pinned node (if pinning)
    gssw_node* pinned_node = nullptr;
    gssw_graph* graph = create_gssw_graph(*align_graph, pinned, &pinned_node);
    
    // perform dynamic programming
    gssw_graph_fill_pinned(graph, align_sequence.c_str(),
                           nt_table, score_matrix,
                           gap_open, gap_extension, full_length_bonus, 15, 2);
    
    // traceback either from pinned position or optimal local alignment
    if (pinned) {
        // trace back pinned alignment
        gssw_graph_mapping** gms = gssw_graph_trace_back_pinned_multi (graph,
                                                                       pinned_node,
                                                                       max_alt_alns,
                                                                       align_sequence.c_str(),
                                                                       align_sequence.size(),
                                                                       nt_table,
                                                                       score_matrix,
                                                                       gap_open,
                                                                       gap_extension,
                                                                       full_length_bonus);
        
        if (pin_left) {
            // translate graph and mappings into original node space
            unreverse_graph(reversed_graph);
            for (int32_t i = 0; i < max_alt_alns; i++) {
                unreverse_graph_mapping(gms[i]);
            }
        }
        
        // convert optimal alignment and store it in the input Alignment object (in the multi alignment,
        // this will have been set to the first in the vector)
        if (gms[0]->score > 0) {
            // have a mapping, can just convert normally
            gssw_mapping_to_alignment(graph, gms[0], alignment, pinned, pin_left, print_score_matrices);
        }
        else if (g.node_size() > 0) {
            // gssw will not identify mappings with 0 score, infer location based on pinning
            
            Mapping* mapping = alignment.mutable_path()->add_mapping();
            mapping->set_rank(1);
            
            // locate at a beginning of an arbitrary source node or end of an arbitrary sink node as appropriate
            Position* position = mapping->mutable_position();
            if (pin_left) {
                position->set_node_id(g.node(0).id());
                position->set_offset(0);
            }
            else {
                position->set_node_id(g.node(g.node_size() - 1).id());
                position->set_offset(g.node(g.node_size() - 1).sequence().length());
            }
            
            // soft clip
            Edit* edit = mapping->add_edit();
            edit->set_to_length(alignment.sequence().length());
            edit->set_sequence(alignment.sequence());
        }
        
        if (multi_alignments) {
            // determine how many non-null alignments were returned
            int32_t num_non_null = max_alt_alns;
            for (int32_t i = 1; i < max_alt_alns; i++) {
                if (gms[i]->score <= 0) {
                    num_non_null = i;
                    break;
                }
            }
            
            // reserve to avoid illegal access errors that occur when the vector reallocates
            multi_alignments->reserve(num_non_null);
            
            // copy the primary alignment
            multi_alignments->emplace_back(alignment);
            
            // convert the alternate alignments and store them at the back of the vector (this will not
            // execute if we are doing single alignment)
            for (int32_t i = 1; i < num_non_null; i++) {
                gssw_graph_mapping* gm = gms[i];
                
                // make new alignment object
                multi_alignments->emplace_back();
                Alignment& next_alignment = multi_alignments->back();
                
                // copy over sequence information from the primary alignment
                next_alignment.set_sequence(alignment.sequence());
                next_alignment.set_quality(alignment.quality());
                
                // get path of the alternate alignment
                gssw_mapping_to_alignment(graph, gm, next_alignment, pinned, pin_left, print_score_matrices);
                
            }
            
        }
        
        for (int32_t i = 0; i < max_alt_alns; i++) {
            gssw_graph_mapping_destroy(gms[i]);
        }
        free(gms);
    }
    else {
        // trace back local alignment
        gssw_graph_mapping* gm = gssw_graph_trace_back (graph,
                                                        align_sequence.c_str(),
                                                        align_sequence.size(),
                                                        nt_table,
                                                        score_matrix,
                                                        gap_open,
                                                        gap_extension);
        
        gssw_mapping_to_alignment(graph, gm, alignment, pinned, pin_left, print_score_matrices);
        gssw_graph_mapping_destroy(gm);
    }
    
    //gssw_graph_print_score_matrices(graph, sequence.c_str(), sequence.size(), stderr);
    
    gssw_graph_destroy(graph);
}

void Aligner::align(Alignment& alignment, Graph& g, bool print_score_matrices) {
    
    align_internal(alignment, nullptr, g, false, false, 1, 0, print_score_matrices);
}

void Aligner::align_pinned(Alignment& alignment, Graph& g, bool pin_left, int8_t full_length_bonus) {
    
    align_internal(alignment, nullptr, g, true, pin_left, 1, full_length_bonus, false);
}

void Aligner::align_pinned_multi(Alignment& alignment, vector<Alignment>& alt_alignments, Graph& g,
                                 bool pin_left, int32_t max_alt_alns, int8_t full_length_bonus) {
    
    if (alt_alignments.size() != 0) {
        cerr << "error:[Aligner::align_pinned_multi] output vector must be empty for pinned multi-aligning" << endl;
        exit(EXIT_FAILURE);
    }
    
    align_internal(alignment, &alt_alignments, g, true, pin_left, max_alt_alns, full_length_bonus, false);
}

void Aligner::align_global_banded(Alignment& alignment, Graph& g,
                                  int32_t band_padding, bool permissive_banding) {
    
    BandedGlobalAligner<int16_t> band_graph = BandedGlobalAligner<int16_t>(alignment,
                                                                           g,
                                                                           band_padding,
                                                                           permissive_banding,
                                                                           false);
    
    band_graph.align(score_matrix, nt_table, gap_open, gap_extension);

}

void Aligner::align_global_banded_multi(Alignment& alignment, vector<Alignment>& alt_alignments, Graph& g,
                                        int32_t max_alt_alns, int32_t band_padding, bool permissive_banding) {
    
    BandedGlobalAligner<int16_t> band_graph = BandedGlobalAligner<int16_t>(alignment,
                                                                           g,
                                                                           alt_alignments,
                                                                           max_alt_alns,
                                                                           band_padding,
                                                                           permissive_banding,
                                                                           false);
    
    band_graph.align(score_matrix, nt_table, gap_open, gap_extension);
}

void Aligner::gssw_mapping_to_alignment(gssw_graph* graph,
                                        gssw_graph_mapping* gm,
                                        Alignment& alignment,
                                        bool pinned,
                                        bool pin_left,
                                        bool print_score_matrices) {
    alignment.clear_path();
    alignment.set_score(gm->score);
    alignment.set_query_position(0);
    Path* path = alignment.mutable_path();
    //alignment.set_cigar(graph_cigar(gm));

    gssw_graph_cigar* gc = &gm->cigar;
    gssw_node_cigar* ncs = gc->elements;
    //cerr << "gm->position " << gm->position << endl;
    string& to_seq = *alignment.mutable_sequence();
    //cerr << "-------------" << endl;

    if (print_score_matrices) {
        gssw_graph_print_score_matrices(graph, to_seq.c_str(), to_seq.size(), stderr);
        //cerr << alignment.DebugString() << endl;
    }
    
    
   
    int graph_cigar_length = gc->length;
    int graph_cigar_start = 0;
    
    // TODO: moving these inserts/deletions can create repeat pinned alignments for multialignments
    // and also artificially lower their score if we take a I/D to get rid of the N rather than an N match
    
    if (pinned) {
         // if the alignment is pinned the final mapping will be to the dummy node, so ignore it
        if (pin_left) {
            graph_cigar_start++;
        }
        else {
            graph_cigar_length--;
        }
        
        // need to detect insertions or deletions that have been recorded on the dummy node and move them onto real nodes
        if (pin_left) {
            gssw_cigar* dummy_node_cigar = ncs[0].cigar;
            
            // was the dummy node's "N" sequence deleted?
            if (dummy_node_cigar->elements[dummy_node_cigar->length - 1].type == 'D') {
                if (dummy_node_cigar->elements[0].type == 'I') {
                    // there is also insert, which must include the dummy N sequence, so remove one
                    dummy_node_cigar->elements[0].length--;
                }
                else {
                    // need to move the deletion to where the dummy N match occurred
                    bool deletion_swapped = false;
                    for (int i = graph_cigar_start; i < graph_cigar_length && !deletion_swapped; i++) {
                        gssw_cigar* cigar = ncs[i].cigar;
                        for (int j = 0; j < cigar->length && !deletion_swapped; j++) {
                            if (cigar->elements[j].type == 'N' || cigar->elements[j].type == 'I') {
                                // we found the dummy N match or insert
                                
                                if (j > 0) {
                                    // there is a deletion preceding it (asssertion below guarantees it's a deletion)
                                    
                                    // increase the length of the deletion
                                    cigar->elements[j - 1].length++;
                                    
                                    if (cigar->elements[j].length > 1) {
                                        // the N-match/insertion is in a run, decrease its length
                                        cigar->elements[j].length--;
                                    }
                                    else {
                                        // overwrite the N match/insertion and move all of the subsequent elements down one position
                                        for (int k = j + 1; k < cigar->length; k++) {
                                            cigar->elements[k - 1] = cigar->elements[k];
                                        }
                                        // adjust the length of the element array
                                        cigar->length--;
                                    }
                                }
                                else if (cigar->elements[j].length == 1 && cigar->elements[j].type == 'N') {
                                    // only one N match, can swap in the single deletion
                                    cigar->elements[j].type = 'D';
                                }
                                else if (cigar->elements[j].length == 1 && cigar->elements[j].type == 'I') {
                                    // one deletion and one insertion cancel each other out, remove this edit
                                    for (int k = j + 1; k < cigar->length; k++) {
                                        cigar->elements[k - 1] = cigar->elements[k];
                                    }
                                    // adjust the length of the element array
                                    cigar->length--;
                                }
                                else {
                                    // more than one N-match/insert, remove one and add the deletion onto the front
                                    cigar->elements[j].length--;
                                    gssw_cigar_push_front(cigar, 'D', 1);
                                }
                                
                                deletion_swapped = true;
                            }
                            else if (cigar->elements[j].type != 'D') {
                                cerr << "error:[Aligner] pinned alignment took a true match before the dummy pinning N-match" << endl;
                                assert(false);
                            }
                        }
                    }
                    
                    assert(deletion_swapped);
                }
            }
            
            // was there an insertion on the dummy node?
            if (dummy_node_cigar->elements[0].type == 'I') {
                // need to move the inserted sequence onto the next node
                if (graph_cigar_start < graph_cigar_length) {
                    // the graph is non-empty
                    gssw_cigar_push_front(ncs[graph_cigar_start].cigar, 'I', dummy_node_cigar->elements[0].length);
                }
            }
        }
        else {
            // repeat the whole routine except with indices reversed for right-pinning
            // TODO: is there a less repetitious way to do this?
            gssw_cigar* dummy_node_cigar = ncs[graph_cigar_length].cigar;
            
            // was the dummy node's "N" sequence deleted?
            if (dummy_node_cigar->elements[0].type == 'D') {
                if (dummy_node_cigar->elements[dummy_node_cigar->length - 1].type == 'I') {
                    // there is also insert, which must include the dummy N sequence, so remove one
                    dummy_node_cigar->elements[dummy_node_cigar->length - 1].length--;
                }
                else {
                    // need to move the deletion to where the dummy N match occurred
                    bool deletion_swapped = false;
                    for (int i = graph_cigar_length - 1; i >= graph_cigar_start && !deletion_swapped; i--) {
                        gssw_cigar* cigar = ncs[i].cigar;
                        for (int j = cigar->length - 1; j >= 0 && !deletion_swapped; j--) {
                            if (cigar->elements[j].type == 'N' || cigar->elements[j].type == 'I') {
                                // we found the dummy N match/insert
                                
                                if (j < cigar->length - 1) {
                                    // there is a deletion preceding this N match/insert (asssertion below guarantees it's a deletion)
                                    
                                    // increase the length of the deletion
                                    cigar->elements[j + 1].length++;
                                    
                                    if (cigar->elements[j].length > 1) {
                                        // the N-match/insert is in a run, decrease its length
                                        cigar->elements[j].length--;
                                    }
                                    else {
                                        // overwrite the N-match/insert and move all of the subsequent elements down one position
                                        for (int k = j + 1; k < cigar->length; k++) {
                                            cigar->elements[k - 1] = cigar->elements[k];
                                        }
                                        // adjust the length of the element array
                                        cigar->length--;
                                    }
                                }
                                else if (cigar->elements[j].length == 1 && cigar->elements[j].type == 'N') {
                                    // only one N match, can swap in the single deletion
                                    cigar->elements[j].type = 'D';
                                }
                                else if (cigar->elements[j].length == 1 && cigar->elements[j].type == 'I') {
                                    // one deletion and one insertion cancel each other out, remove this edit
                                    for (int k = j + 1; k < cigar->length; k++) {
                                        cigar->elements[k - 1] = cigar->elements[k];
                                    }
                                    // adjust the length of the element array
                                    cigar->length--;
                                }
                                else {
                                    // more than one N-match/insert, remove one and add the deletion onto the front
                                    cigar->elements[j].length--;
                                    gssw_cigar_push_back(cigar, 'D', 1);
                                }
                                
                                deletion_swapped = true;
                            }
                            else if (cigar->elements[j].type != 'D') {
                                cerr << "error:[Aligner] pinned alignment took a true match before the dummy N-match" << endl;
                                assert(false);
                            }
                        }
                    }
                    
                    assert(deletion_swapped);
                }
            }
            
            // was there an insertion on the dummy node?
            if (dummy_node_cigar->elements[dummy_node_cigar->length - 1].type == 'I') {
                // need to move the inserted sequence onto the next node
                if (graph_cigar_start < graph_cigar_length) {
                    // the graph is non-empty
                    gssw_cigar_push_back(ncs[graph_cigar_length - 1].cigar, 'I', dummy_node_cigar->elements[dummy_node_cigar->length - 1].length);
                }
            }
        }
    }
    
    int to_pos = 0;
    int from_pos = gm->position;
    
    for (int i = graph_cigar_start; i < graph_cigar_length; ++i) {
        
        // check that the current alignment has a non-zero length
        gssw_cigar* c = ncs[i].cigar;
        int l = c->length;
        if (l == 0) continue;
        gssw_cigar_element* e = c->elements;

        Node* from_node = (Node*) ncs[i].node->data;
        string& from_seq = *from_node->mutable_sequence();
        Mapping* mapping = path->add_mapping();
        
        if (i > graph_cigar_start){
             // reset for each node after the first
            from_pos = 0;
        }
        
        mapping->mutable_position()->set_node_id(ncs[i].node->id);
        mapping->mutable_position()->set_offset(from_pos);
        mapping->set_rank(path->mapping_size());

        //cerr << from_node->id() << ":" << endl;

        for (int j=0; j < l; ++j, ++e) {
            int32_t length = e->length;
            //cerr << e->length << e->type << endl;
            
            Edit* edit;
            switch (e->type) {
            case 'M':
            case 'X':
            case 'N': {
                // do the sequences match?
                // emit a stream of "SNPs" and matches
                int h = from_pos;
                int last_start = from_pos;
                int k = to_pos;
                for ( ; h < from_pos + length; ++h, ++k) {
                    //cerr << h << ":" << k << " " << from_seq[h] << " " << to_seq[k] << endl;
                    if (from_seq[h] != to_seq[k]) {
                        // emit the last "match" region
                        if (h - last_start > 0) {
                            edit = mapping->add_edit();
                            edit->set_from_length(h-last_start);
                            edit->set_to_length(h-last_start);
                        }
                        // set up the SNP
                        edit = mapping->add_edit();
                        edit->set_from_length(1);
                        edit->set_to_length(1);
                        edit->set_sequence(to_seq.substr(k,1));
                        last_start = h+1;
                    }
                }
                // handles the match at the end or the case of no SNP
                if (h - last_start > 0) {
                    edit = mapping->add_edit();
                    edit->set_from_length(h-last_start);
                    edit->set_to_length(h-last_start);
                }
                to_pos += length;
                from_pos += length;
            } break;
            case 'D':
                edit = mapping->add_edit();
                edit->set_from_length(length);
                edit->set_to_length(0);
                from_pos += length;
                break;
            case 'I':
                edit = mapping->add_edit();
                edit->set_from_length(0);
                edit->set_to_length(length);
                edit->set_sequence(to_seq.substr(to_pos, length));
                to_pos += length;
                break;
            case 'S':
                // note that soft clips and insertions are semantically equivalent
                // and can only be differentiated by their position in the read
                // with soft clips coming at the start or end
                edit = mapping->add_edit();
                edit->set_from_length(0);
                edit->set_to_length(length);
                edit->set_sequence(to_seq.substr(to_pos, length));
                to_pos += length;
                break;
            default:
                cerr << "error:[Aligner::gssw_mapping_to_alignment] "
                     << "unsupported cigar op type " << e->type << endl;
                exit(1);
                break;
                 
            }
        }
    }
    
    // compute and set identity
    alignment.set_identity(identity(alignment.path()));
}

void Aligner::reverse_graph(Graph& g, Graph& reversed_graph_out) {
    if (reversed_graph_out.node_size()) {
        cerr << "error:[Aligner::reverse_graph] output graph is not empty" << endl;
        exit(EXIT_FAILURE);
    }
    
    // add reversed nodes in reverse order (Graphs come in topologically sorted and gssw
    // depends on this fact)
    for (int64_t i = g.node_size() - 1; i >= 0; i--) {
        const Node& original_node = g.node(i);
        
        Node* reversed_node = reversed_graph_out.add_node();
        
        // reverse the sequence
        string* reversed_node_seq = reversed_node->mutable_sequence();
        reversed_node_seq->resize(original_node.sequence().length());
        reverse_copy(original_node.sequence().begin(), original_node.sequence().end(), reversed_node_seq->begin());
        
        // preserve ids for easier translation
        reversed_node->set_id(original_node.id());
    }
    
    // add reversed edges
    for (int64_t i = 0; i < g.edge_size(); i++) {
        const Edge& original_edge = g.edge(i);
        
        Edge* reversed_edge = reversed_graph_out.add_edge();
        
        // reverse edge orientation
        reversed_edge->set_from(original_edge.to());
        reversed_edge->set_to(original_edge.from());
        
        // we will swap the 5'/3' labels of the node ends after reversing the sequence so that
        // an edge leaving an end now enters a beginning and an edge entering a beginning now
        // leaves an end
        reversed_edge->set_from_start(original_edge.to_end());
        reversed_edge->set_to_end(original_edge.from_start());
    }

}

void Aligner::unreverse_graph(Graph& graph) {
    // this is only for getting correct reference-relative edits, so we can get away with only
    // reversing the sequences and not paying attention to the edges
    for (int64_t i = 0; i < graph.node_size(); i++) {
        Node* node = graph.mutable_node(i);
        string* node_seq = node->mutable_sequence();
        reverse(node_seq->begin(), node_seq->end());
    }
}

void Aligner::unreverse_graph_mapping(gssw_graph_mapping* gm) {
    
    gssw_graph_cigar* graph_cigar = &(gm->cigar);
    gssw_node_cigar* node_cigars = graph_cigar->elements;
    
    // reverse the order of the node cigars
    int32_t num_switching_nodes = graph_cigar->length / 2;
    int32_t last_idx = graph_cigar->length - 1;
    for (int32_t i = 0; i < num_switching_nodes; i++) {
        swap(node_cigars[i], node_cigars[last_idx - i]);
    }
    
    // reverse the actual cigar string for each node cigar
    for (int32_t i = 0; i < graph_cigar->length; i++) {
        gssw_cigar* node_cigar = node_cigars[i].cigar;
        gssw_cigar_element* elements = node_cigar->elements;
        
        int32_t num_switching_elements = node_cigar->length / 2;
        last_idx = node_cigar->length - 1;
        for (int32_t j = 0; j < num_switching_elements; j++) {
            swap(elements[j], elements[last_idx - j]);
        }
    }
    
    // compute the position in the first node
    if (graph_cigar->length > 0) {
        gssw_cigar_element* first_node_elements = node_cigars[0].cigar->elements;
        int32_t num_first_node_elements = node_cigars[0].cigar->length;
        uint32_t num_ref_aligned = 0; // the number of characters on the node sequence that are aligned
        for (int32_t i = 0; i < num_first_node_elements; i++) {
            switch (first_node_elements[i].type) {
                case 'M':
                case 'X':
                case 'N':
                case 'D':
                    num_ref_aligned += first_node_elements[i].length;
                    break;
                    
            }
        }
        gm->position = node_cigars[0].node->len - num_ref_aligned;
    }
    else {
        gm->position = 0;
    }
}

string Aligner::graph_cigar(gssw_graph_mapping* gm) {
    stringstream s;
    gssw_graph_cigar* gc = &gm->cigar;
    gssw_node_cigar* nc = gc->elements;
    int to_pos = 0;
    int from_pos = gm->position;
    //string& to_seq = *alignment.mutable_sequence();
    s << from_pos << '@';
    for (int i = 0; i < gc->length; ++i, ++nc) {
        if (i > 0) from_pos = 0; // reset for each node after the first
        Node* from_node = (Node*) nc->node->data;
        s << from_node->id() << ':';
        gssw_cigar* c = nc->cigar;
        int l = c->length;
        gssw_cigar_element* e = c->elements;
        for (int j=0; j < l; ++j, ++e) {
            s << e->length << e->type;
        }
        if (i + 1 < gc->length) {
            s << ",";
        }
    }
    return s.str();
}

void Aligner::init_mapping_quality(double gc_content) {
    log_base = gssw_dna_recover_log_base(match, mismatch, gc_content, 1e-12);
}

bool Aligner::is_mapping_quality_initialized() {
    return (log_base <= 0.0);
}

double Aligner::maximum_mapping_quality_exact(vector<double>& scaled_scores, size_t* max_idx_out) {
    size_t size = scaled_scores.size();
        
    // if necessary, assume a null alignment of 0.0 for comparison since this is local
    if (size == 1) {
        scaled_scores.push_back(0.0);
    }
    
    double max_score = scaled_scores[0];
    size_t max_idx = 0;
    for (size_t i = 1; i < size; i++) {
        if (scaled_scores[i] > max_score) {
            max_score = scaled_scores[i];
            max_idx = i;
        }
    }
    
    *max_idx_out = max_idx;
    
    if (max_score * size < exp_overflow_limit) {
        // no risk of double overflow, sum exp directly (half as many transcendental function evals)
        double numer = 0.0;
        for (size_t i = 0; i < size; i++) {
            if (i == max_idx) {
                continue;
            }
            numer += exp(scaled_scores[i]);
        }
        return -10.0 * log10(numer / (numer + exp(scaled_scores[max_idx])));
    }
    else {
        // work in log transformed valued to avoid risk of overflow
        double log_sum_exp = scaled_scores[0];
        for (size_t i = 1; i < size; i++) {
            log_sum_exp = add_log(log_sum_exp, scaled_scores[i]);
        }
        return -10.0 * log10(1.0 - exp(scaled_scores[max_idx] - log_sum_exp));
    }
}

// TODO: this algorithm has numerical problems that would be difficult to solve without increasing the
// time complexity: adding the probability of the maximum likelihood tends to erase the contribution
// of the other terms so that when you subtract them off you get scores of 0 or infinity

//vector<double> Aligner::all_mapping_qualities_exact(vector<double>& scaled_scores) {
//    
//    double max_score = *max_element(scaled_scores.begin(), scaled_scores.end());
//    size_t size = scaled_scores.size();
//    
//    vector<double> mapping_qualities(size);
//    
//    if (max_score * size < exp_overflow_limit) {
//        // no risk of double overflow, sum exp directly (half as many transcendental function evals)
//        vector<double> exp_scaled_scores(size);
//        for (size_t i = 0; i < size; i++) {
//            exp_scaled_scores[i] = exp(scaled_scores[i]);
//        }
//        double denom = std::accumulate(exp_scaled_scores.begin(), exp_scaled_scores.end(), 0.0);
//        for (size_t i = 0; i < size; i++) {
//            mapping_qualities[i] = -10.0 * log10((denom - exp_scaled_scores[i]) / denom);
//        }
//    }
//    else {
//        // work in log transformed valued to avoid risk of overflow
//        double log_sum_exp = scaled_scores[0];
//        for (size_t i = 1; i < size; i++) {
//            log_sum_exp = add_log(log_sum_exp, scaled_scores[i]);
//        }
//        for (size_t i = 0; i < size; i++) {
//            mapping_qualities[i] = -10.0 * log10(1.0 - exp(scaled_scores[i] - log_sum_exp));
//        }
//    }
//    return mapping_qualities;
//}

double Aligner::maximum_mapping_quality_approx(vector<double>& scaled_scores, size_t* max_idx_out) {

    // if necessary, assume a null alignment of 0.0 for comparison since this is local
    if (scaled_scores.size() == 1) {
        scaled_scores.push_back(0.0);
    }

    double max_score = scaled_scores[0];
    size_t max_idx = 0;

    double next_score = -std::numeric_limits<double>::max();
    int32_t next_count = 0;

    for (int32_t i = 1; i < scaled_scores.size(); i++) {
        double score = scaled_scores[i];
        if (score > max_score) {
            if (next_score == max_score) {
                next_count++;
            }
            else {
                next_score = max_score;
                next_count = 1;
            }
            max_score = score;
            max_idx = i;
        }
        else if (score > next_score) {
            next_score = score;
            next_count = 1;
        }
        else if (score == next_score) {
            next_count++;
        }
    }

    *max_idx_out = max_idx;

    return max(0.0, quality_scale_factor * (max_score - next_score - (next_count > 1 ? log(next_count) : 0.0)));

}

void Aligner::compute_mapping_quality(vector<Alignment>& alignments,
                                      int max_mapping_quality,
                                      bool fast_approximation,
                                      double cluster_mq,
                                      bool use_cluster_mq) {

    if (log_base <= 0.0) {
        cerr << "error:[Aligner] must call init_mapping_quality before computing mapping qualities" << endl;
        exit(EXIT_FAILURE);
    }
    
    size_t size = alignments.size();
    
    if (size == 0) {
        return;
    }

    vector<double> scaled_scores(size);
    for (size_t i = 0; i < size; i++) {
        scaled_scores[i] = log_base * alignments[i].score();
    }
    
    double mapping_quality;
    size_t max_idx;
    if (!fast_approximation) {
        mapping_quality = maximum_mapping_quality_exact(scaled_scores, &max_idx);
    }
    else {
        mapping_quality = maximum_mapping_quality_approx(scaled_scores, &max_idx);
    }

    if (mapping_quality > max_mapping_quality) {
        mapping_quality = max_mapping_quality;
    }

    if (use_cluster_mq) {
        mapping_quality = prob_to_phred(sqrt(phred_to_prob(cluster_mq + mapping_quality)));
    }

    alignments[max_idx].set_mapping_quality((int32_t) round(mapping_quality));
}

void Aligner::compute_paired_mapping_quality(pair<vector<Alignment>, vector<Alignment>>& alignment_pairs,
                                             int max_mapping_quality,
                                             bool fast_approximation,
                                             double cluster_mq,
                                             bool use_cluster_mq) {

    if (log_base <= 0.0) {
        cerr << "error:[Aligner] must call init_mapping_quality before computing mapping qualities" << endl;
        exit(EXIT_FAILURE);
    }
    
    size_t size = min(
        alignment_pairs.first.size(),
        alignment_pairs.second.size());
    
    if (size == 0) {
        return;
    }
    
    vector<double> scaled_scores(size);

    for (size_t i = 0; i < size; i++) {
        scaled_scores[i] = log_base * (alignment_pairs.first[i].score() + alignment_pairs.second[i].score());
    }
    
    size_t max_idx;
    double mapping_quality;
    if (!fast_approximation) {
        mapping_quality = maximum_mapping_quality_exact(scaled_scores, &max_idx);
    }
    else {
        mapping_quality = maximum_mapping_quality_approx(scaled_scores, &max_idx);
    }

    if (mapping_quality > max_mapping_quality) {
        mapping_quality = max_mapping_quality;
    }

    if (use_cluster_mq) {
        mapping_quality = prob_to_phred(sqrt(phred_to_prob(cluster_mq + mapping_quality)));
    }

    alignment_pairs.first[max_idx].set_mapping_quality((int32_t) round(mapping_quality));
    alignment_pairs.second[max_idx].set_mapping_quality((int32_t) round(mapping_quality));
}

int32_t Aligner::score_exact_match(const string& sequence) {
    return match * sequence.length();
}

double Aligner::score_to_unnormalized_likelihood_ln(double score) {
    // Log base needs to be set, or this can't work. It's set by default in
    // QualAdjAligner but needs to be set up manually in the normal Aligner.
    assert(log_base != 0);
    // Likelihood is proportional to e^(lambda * score), so ln is just the exponent.
    return log_base * score; 
}

QualAdjAligner::QualAdjAligner(int8_t _match,
                               int8_t _mismatch,
                               int8_t _gap_open,
                               int8_t _gap_extension,
                               int8_t _max_scaled_score,
                               uint8_t _max_qual_score,
                               double gc_content) : Aligner(_match, _mismatch, _gap_open, _gap_extension) {
    
    init_quality_adjusted_scores(_max_scaled_score, _max_qual_score, gc_content);
}

void QualAdjAligner::init_quality_adjusted_scores(int8_t _max_scaled_score,
                                                  uint8_t _max_qual_score,
                                                  double gc_content) {
    max_qual_score = _max_qual_score;
    scaled_gap_open = gap_open;
    scaled_gap_extension = gap_extension;
    
    adjusted_score_matrix = gssw_dna_scaled_adjusted_qual_matrix(_max_scaled_score, max_qual_score, &scaled_gap_open,
                                                                 &scaled_gap_extension, match, mismatch,
                                                                 gc_content, 1e-12);
    init_mapping_quality(gc_content);
}

void QualAdjAligner::init_mapping_quality(double gc_content) {
    log_base = gssw_dna_recover_log_base(match, mismatch, gc_content, 1e-12);
    // adjust to scaled matrix (a bit hacky but correct)
    log_base /= (scaled_gap_open / gap_open);
}

QualAdjAligner::~QualAdjAligner(void) {
    free(adjusted_score_matrix);
}

void QualAdjAligner::align_internal(Alignment& alignment, vector<Alignment>* multi_alignments, Graph& g,
                                    bool pinned, bool pin_left, int32_t max_alt_alns, int8_t full_length_bonus,
                                    bool print_score_matrices) {
    
    // check input integrity
    if (pin_left && !pinned) {
        cerr << "error:[Aligner] cannot choose pinned end in non-pinned alignment" << endl;
        exit(EXIT_FAILURE);
    }
    if (multi_alignments && !pinned) {
        cerr << "error:[Aligner] multiple traceback is not implemented in local alignment, only pinned and global" << endl;
        exit(EXIT_FAILURE);
    }
    if (!multi_alignments && max_alt_alns != 1) {
        cerr << "error:[Aligner] cannot specify maximum number of alignments in single alignment" << endl;
        exit(EXIT_FAILURE);
    }
    
    // scale up the full length bonus
    full_length_bonus *= (scaled_gap_open / gap_open);
    
    // alignment pinning algorithm is based on pinning in bottom right corner, if pinning in top
    // left we need to reverse all the sequences first and translate the alignment back later
    
    // create reversed graph if necessary
    Graph reversed_graph;
    if (pin_left) {
        reverse_graph(g, reversed_graph);
    }
    
    // choose forward or reversed objects
    // note: have to make copies of the strings because we will modify them to add a pinning point
    Graph* align_graph = &g;
    string align_sequence = alignment.sequence();
    string align_quality = alignment.quality();
    if (pin_left) {
        align_graph = &reversed_graph;
        reverse(align_sequence.begin(), align_sequence.end());
        reverse(align_quality.begin(), align_quality.end());
    }
    
    if (pinned) {
        if (pin_left) {
            align_graph = &reversed_graph;
        }
        
        // add dummy ending to align to dummy pinned node
        align_sequence += "N";
        align_quality += (char) 0;
    }
    
    if (align_quality.length() != align_sequence.length()) {
        cerr << "error:[Aligner] sequence and quality strings different lengths, cannot perform base quality adjusted alignment" << endl;
        exit(EXIT_FAILURE);
    }
    
    // convert into gssw graph and get dummy pinned node (if pinning)
    gssw_node* pinned_node = nullptr;
    gssw_graph* graph = create_gssw_graph(*align_graph, pinned, &pinned_node);
    
    // perform dynamic programming
    gssw_graph_fill_pinned_qual_adj(graph, align_sequence.c_str(), align_quality.c_str(),
                                    nt_table, adjusted_score_matrix,
                                    scaled_gap_open, scaled_gap_extension, full_length_bonus, 15, 2);
    
    // traceback either from pinned position or optimal local alignment
    if (pinned) {
        // trace back pinned alignment
        gssw_graph_mapping** gms = gssw_graph_trace_back_pinned_qual_adj_multi (graph,
                                                                                pinned_node,
                                                                                max_alt_alns,
                                                                                align_sequence.c_str(),
                                                                                align_quality.c_str(),
                                                                                align_sequence.size(),
                                                                                nt_table,
                                                                                adjusted_score_matrix,
                                                                                scaled_gap_open,
                                                                                scaled_gap_extension,
                                                                                full_length_bonus);
        
        if (pin_left) {
            // translate graph and mappings into original node space
            unreverse_graph(reversed_graph);
            for (int32_t i = 0; i < max_alt_alns; i++) {
                unreverse_graph_mapping(gms[i]);
            }
        }
        
        // convert optimal alignment and store it in the input Alignment object (in the multi alignment,
        // this will have been set to the first in the vector)
        if (gms[0]->score > 0) {
            // have a mapping, can just convert normally
            gssw_mapping_to_alignment(graph, gms[0], alignment, pinned, pin_left, print_score_matrices);
        }
        else if (g.node_size() > 0) {
            // gssw will not identify mappings with 0 score, infer location based on pinning
            
            Mapping* mapping = alignment.mutable_path()->add_mapping();
            mapping->set_rank(1);
            
            // locate at a beginning of a source node or end of a sink node as appropriate
            Position* position = mapping->mutable_position();
            if (pin_left) {
                position->set_node_id(g.node(0).id());
                position->set_offset(0);
            }
            else {
                position->set_node_id(g.node(g.node_size() - 1).id());
                position->set_offset(g.node(g.node_size() - 1).sequence().length());
            }
            
            // soft clip
            Edit* edit = mapping->add_edit();
            edit->set_to_length(alignment.sequence().length());
            edit->set_sequence(alignment.sequence());
        }
        
        
        if (multi_alignments) {
            // determine how many non-null alignments were returned
            int32_t num_non_null = max_alt_alns;
            for (int32_t i = 1; i < max_alt_alns; i++) {
                if (gms[i]->score <= 0) {
                    num_non_null = i;
                    break;
                }
            }
            
            // reserve to avoid illegal access errors that occur when the vector reallocates
            multi_alignments->reserve(num_non_null);
            
            // copy the primary alignment
            multi_alignments->emplace_back(alignment);
            
            // convert the alternate alignments and store them at the back of the vector (this will not
            // execute if we are doing single alignment)
            for (int32_t i = 1; i < num_non_null; i++) {
                gssw_graph_mapping* gm = gms[i];
                
                // make new alignment object
                multi_alignments->emplace_back();
                Alignment& next_alignment = multi_alignments->back();
                
                // copy over sequence information from the primary alignment
                next_alignment.set_sequence(alignment.sequence());
                next_alignment.set_quality(alignment.quality());
                
                // get path of the alternate alignment
                gssw_mapping_to_alignment(graph, gm, next_alignment, pinned, pin_left, print_score_matrices);
                
            }
        }
        
        for (int32_t i = 0; i < max_alt_alns; i++) {
            gssw_graph_mapping_destroy(gms[i]);
        }
        free(gms);
    }
    else {
        // trace back local alignment
        gssw_graph_mapping* gm = gssw_graph_trace_back_qual_adj (graph,
                                                                 align_sequence.c_str(),
                                                                 align_quality.c_str(),
                                                                 align_sequence.size(),
                                                                 nt_table,
                                                                 adjusted_score_matrix,
                                                                 scaled_gap_open,
                                                                 scaled_gap_extension);
        
        gssw_mapping_to_alignment(graph, gm, alignment, pinned, pin_left, print_score_matrices);
        gssw_graph_mapping_destroy(gm);
    }
    
    //gssw_graph_print_score_matrices(graph, sequence.c_str(), sequence.size(), stderr);
    
    gssw_graph_destroy(graph);
    
}

void QualAdjAligner::align(Alignment& alignment, Graph& g, bool print_score_matrices) {
    
    align_internal(alignment, nullptr, g, false, false, 1, 0, print_score_matrices);
}

void QualAdjAligner::align_pinned(Alignment& alignment, Graph& g, bool pin_left, int8_t full_length_bonus) {

    align_internal(alignment, nullptr, g, true, pin_left, 1, full_length_bonus, false);

}

void QualAdjAligner::align_pinned_multi(Alignment& alignment, vector<Alignment>& alt_alignments, Graph& g,
                                        bool pin_left, int32_t max_alt_alns, int8_t full_length_bonus) {
    align_internal(alignment, &alt_alignments, g, true, pin_left, max_alt_alns, full_length_bonus, false);
}

void QualAdjAligner::align_global_banded(Alignment& alignment, Graph& g,
                                  int32_t band_padding, bool permissive_banding) {
    
    BandedGlobalAligner<int16_t> band_graph = BandedGlobalAligner<int16_t>(alignment,
                                                                           g,
                                                                           band_padding,
                                                                           permissive_banding,
                                                                           true);
    
    band_graph.align(adjusted_score_matrix, nt_table, scaled_gap_open, scaled_gap_extension);
    
}

void QualAdjAligner::align_global_banded_multi(Alignment& alignment, vector<Alignment>& alt_alignments, Graph& g,
                                               int32_t max_alt_alns, int32_t band_padding, bool permissive_banding) {
    
    BandedGlobalAligner<int16_t> band_graph = BandedGlobalAligner<int16_t>(alignment,
                                                                           g,
                                                                           alt_alignments,
                                                                           max_alt_alns,
                                                                           band_padding,
                                                                           permissive_banding,
                                                                           true);
    
    band_graph.align(adjusted_score_matrix, nt_table, scaled_gap_open, scaled_gap_extension);
    
}

int32_t QualAdjAligner::score_exact_match(const string& sequence, const string& base_quality) {
    int32_t score = 0;
    for (int32_t i = 0; i < sequence.length(); i++) {
        // index 5 x 5 score matrices (ACGTN)
        // always have match so that row and column index are same and can combine algebraically
        score += adjusted_score_matrix[25 * base_quality[i] + 6 * nt_table[sequence[i]]];
    }
    return score;
}
