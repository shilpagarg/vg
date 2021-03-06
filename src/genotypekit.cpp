#include "genotypekit.hpp"

namespace vg {

using namespace std;


CactusSiteFinder::CactusSiteFinder(VG& graph, const string& hint_path_name): graph(graph), hint_path_name(hint_path_name) {
    // Make sure the graph is sorted.
    // cactus needs the nodes to be sorted in order to find a source and sink.
    graph.sort();
}

void CactusSiteFinder::for_each_site_parallel(const function<void(const NestedSite)>& lambda) {
    
    // Get the bubble tree in Cactus format
    BubbleTree* bubble_tree = ultrabubble_tree(graph);

    // Convert to NestedSites
    
    // We use this to hold the NestedSites that are children until their parents
    // are ready to be converted.
    map<BubbleTree::Node*, NestedSite> converted_children;

    bubble_tree->for_each_postorder([&](BubbleTree::Node* node) {
        // Process children before parents so we can embed them in the parent.
        
        Bubble& bubble = node->v;
        if (node != bubble_tree->root) {
            // If we aren't the root node of the tree, we need to be a NestedSite
            
            // We're going to fill in this NestedSite.
            NestedSite& to_fill = converted_children[node];
            
            // Set up the start and end
            NodeTraversal start(graph.get_node(bubble.start.node), !bubble.start.is_end);
            NodeTraversal end(graph.get_node(bubble.end.node), bubble.end.is_end);
            // Make sure to preserve original endpoint
            // ordering, because swapping them without flipping their
            // orientation flags will make an inside-out site.
            to_fill.start = start;
            to_fill.end = end;
            
            for(id_t node_id : bubble.contents) {
                // Convert all the directly contained nodes to pointers
                to_fill.nodes.insert(graph.get_node(node_id));
            }
            
            for(BubbleTree::Node* child_node : node->children) {
                // Attach all the children by moving them out of our map.
                assert(converted_children.count(child_node));
                to_fill.children.emplace_back(std::move(converted_children[child_node]));
                converted_children.erase(child_node);
                
                // Fill in child borders with the NodeTraversals leading into the children.
                to_fill.child_border_index[to_fill.children.back().start] = to_fill.children.size() - 1;
                to_fill.child_border_index[to_fill.children.back().end.reverse()] = to_fill.children.size() - 1;
                
            }
            
            // Now do all the edges
            
            for(Node* internal_node : to_fill.nodes) {
                // Collect edges on internal nodes
                if(internal_node == to_fill.start.node || internal_node == to_fill.end.node) {
                    // Look only at internal nodes (not start or end of site)
                    continue;
                }
                
                // Since these aren't the start or end nodes of either this site
                // or any child site, all the edges on them must be part of this
                // site.
                auto node_edges = graph.edges_of(internal_node);
                // Dump them all in
                to_fill.edges.insert(node_edges.begin(), node_edges.end());
            }

            for(auto& child : to_fill.children) {
                // Now do edges between sites linked by just an edge. For each child site...
                
                // Get the outer side of the start traversal
                NodeSide start_outer_side = NodeSide(child.start.node->id(), child.start.backward);
                
                // Get the connected sides
                auto start_connected_sides = graph.sides_of(start_outer_side);
                
                for(auto& connected_side : start_connected_sides) {
                    // Pull in all the edges between the outer side of the
                    // contained site and everything else. They must be within
                    // this parent site.
                    to_fill.edges.insert(graph.get_edge(start_outer_side, connected_side));
                }
                
                // Repeat for the end of the child site
                
                // The outer side of the end of the site is a right side if the
                // child side doesn't end with a backwards node.
                NodeSide end_outer_side = NodeSide(child.end.node->id(), !child.end.backward);
                auto end_connected_sides = graph.sides_of(end_outer_side);
                for(auto& connected_side : end_connected_sides) {
                    to_fill.edges.insert(graph.get_edge(end_outer_side, connected_side));
                }
                
            }
            
            // Finally do edges on the inside sides of this site's start and
            // end. Those are the only ones not yet covered.
            NodeSide start_inner_side = NodeSide(to_fill.start.node->id(), !to_fill.start.backward);
            for(auto& connected_side : graph.sides_of(start_inner_side)) {
                // Wire up the inner side of the start node to all its connected sides
                to_fill.edges.insert(graph.get_edge(start_inner_side, connected_side));
            }
            
            NodeSide end_inner_side = NodeSide(to_fill.end.node->id(), to_fill.end.backward);
            for(auto& connected_side : graph.sides_of(end_inner_side)) {
                // Wire up the inner side of the end node to all its connected sides
                to_fill.edges.insert(graph.get_edge(end_inner_side, connected_side));
            }
            
            // Now we're done with all the edges.
            
        } 
    });

    delete bubble_tree;
    
    // Now emit all the top-level sites
    
    for(auto& child_and_site : converted_children) {
    
        // OpenMP doesn't like to let us use the reference, even though
        // we know it will survive the tasks. We grab a pointer to make
        // it happy.
        auto* lambda_pointer = &lambda;
        
        // Ditto for the reference into converted_children
        auto* site = &child_and_site.second;
        
        // Operate on the site. Make sure to move into task stack storage.
        #pragma omp task
        {
            (*lambda_pointer)(std::move(*site));
        }
        
    }
        
    // Don't return until all the lambda tasks are done.
    #pragma omp taskwait    
    
}
    
// add all node traversals that valid walks from this one onto a stack
void stack_up_valid_walks(VG& graph, NodeTraversal walk_head, vector<NodeTraversal>& stack) {
    
    id_t head_id = walk_head.node->id();
    
    if (walk_head.backward) {
        // we are leaving from the start of the node
        
        // get all edges involving this node so we can filter them down to valid walks
        for (Edge* edge : graph.edges_of(walk_head.node)) {
            if (edge->from() == head_id && edge->from_start()) {
                // the edge is part of a valid walk
                Node* next_node = graph.get_node(edge->to());
                bool next_backward = edge->to_end();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
            else if (edge->to() == head_id && !edge->to_end()) {
                // the edge is part of a valid walk in the opposite orientation
                Node* next_node = graph.get_node(edge->from());
                bool next_backward = edge->from_start();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
        }
    }
    else {
        // we are leaving from the end of the node
        
        // get all edges involving this node so we can filter them down to valid walks
        for (Edge* edge : graph.edges_of(walk_head.node)) {
            if (edge->from() == head_id && !edge->from_start()) {
                // the edge is part of a valid walk
                Node* next_node = graph.get_node(edge->to());
                bool next_backward = edge->to_end();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
            else if (edge->to() == head_id && edge->to_end()) {
                // the edge is part of a valid walk in the opposite orientation
                Node* next_node = graph.get_node(edge->from());
                bool next_backward = edge->from_start();
                // add the next traversal in the walk to the stack
                stack.push_back(NodeTraversal(next_node, next_backward));
            }
        }
    }
}
    
ExhaustiveTraversalFinder::ExhaustiveTraversalFinder(VG& graph) : graph(graph) {
    // nothing more to do
}
    
ExhaustiveTraversalFinder::~ExhaustiveTraversalFinder() {
    // no heap objects
}
    
vector<SiteTraversal> ExhaustiveTraversalFinder::find_traversals(const NestedSite& site) {

    vector<SiteTraversal> to_return;
    
    // construct maps that lets us "skip over" child sites
    map<NodeTraversal, const NestedSite*> child_site_starts;
    map<NodeTraversal, const NestedSite*> child_site_ends;
    for (auto iter = site.children.begin(); iter != site.children.end(); iter++) {
        const NestedSite* subsite = &(*iter);
        child_site_starts[subsite->start] = subsite;
        // reverse the direction of the end because we want to find it when we're entering
        // the site from that direction
        child_site_ends[NodeTraversal(subsite->end.node, !subsite->end.backward)] = subsite;
    }
    
    // keeps track of the walk of the DFS traversal
    list<SiteTraversal::Visit> path;
    
    // these mark the start of the edges out of the node that is on the head of the path
    // they can be used to see how many nodes we need to peel off the path when we're
    // backtracking
    NodeTraversal stack_sentinel = NodeTraversal(nullptr);
    
    // initialize stack for DFS traversal of site
    vector<NodeTraversal> stack;
    stack.push_back(site.start);
    
    while (stack.size()) {
        
        NodeTraversal node_traversal = stack.back();
        stack.pop_back();
        
        // we have traversed all of edges out of the head of the path, so we can pop it off
        if (node_traversal == stack_sentinel) {
            path.pop_back();
            continue;
        }
        
        // initialize the visit with the node traversal
        SiteTraversal::Visit visit(node_traversal);
        // have we finished a traversal through the site?
        if (node_traversal == site.end) {
            // add it to path
            path.push_back(visit);
            
            // yield path as a site traversal
            SiteTraversal traversal;
            to_return.push_back(traversal);
            to_return.back().visits = path;
            
            // remove the final visit
            path.pop_back();
            
            // don't proceed to add more onto the DFS stack
            continue;
        }
        
        // mark the beginning of this node/site's edges forward in the stack
        stack.push_back(stack_sentinel);
        
        if (child_site_starts.count(node_traversal)) {
            // make a visit out of the site
            const NestedSite* child_site = child_site_starts[node_traversal];
            visit.child = child_site;
            visit.node = nullptr;
            visit.backward = false;
            
            // skip the site and add the other side to the stack
            stack.push_back(child_site->end);
        }
        else if (child_site_ends.count(node_traversal)) {
            // make a visit out of the site
            const NestedSite* child_site = child_site_ends[node_traversal];
            visit.child = child_site;
            visit.node = nullptr;
            visit.backward = true;
            
            // we're traveling through the site backwards, so reverse the
            // traversal on the start end
            NodeTraversal reverse_start = NodeTraversal(child_site->start.node,
                                                        !child_site->start.backward);
            
            // skip the site and add the other side to the stack
            stack.push_back(reverse_start);
        }
        else {
            // add all of the node traversals we can reach through valid walks
            stack_up_valid_walks(graph, node_traversal, stack);
        }
        
        // add visit to path
        path.push_back(visit);
    }
    
    return to_return;
}
    
ReadRestrictedTraversalFinder::ReadRestrictedTraversalFinder(VG& graph,
                                                             const map<string, Alignment*>& reads_by_name,
                                                             int min_recurrence, int max_path_search_steps) :
                                                             graph(graph), reads_by_name(reads_by_name),
                                                             min_recurrence(min_recurrence),
                                                             max_path_search_steps(max_path_search_steps) {
    // nothing else to do
}

ReadRestrictedTraversalFinder::~ReadRestrictedTraversalFinder() {
    // no heap variables
}
    
// replaces get_paths_through_site
vector<SiteTraversal> ReadRestrictedTraversalFinder::find_traversals(const NestedSite& site) {
    // We're going to emit traversals supported by any paths in the graph.
    
    // Put all our subpaths in here to deduplicate them by sequence they spell
    // out. And to count occurrences. Note that the occurrence count will be
    // boosted to min_recurrence if a non-read path in the graph supports a
    // certain traversal string, so we don't end up dropping unsupported ref
    // alleles.
    map<string, pair<list<SiteTraversal::Visit>, int>> results;
    
    // construct maps that lets us "skip over" child sites
    map<NodeTraversal, const NestedSite*> child_site_starts;
    map<NodeTraversal, const NestedSite*> child_site_ends;
    for (const NestedSite& subsite : site.children) {
        child_site_starts[subsite.start] = &subsite;
        // reverse the direction of the end because we want to find it when we're entering
        // the site from that direction
        NodeTraversal reverse_end = NodeTraversal(subsite.end.node, !subsite.end.backward);
        child_site_ends[reverse_end] = &subsite;
    }
    
#ifdef debug
#pragma omp critical (cerr)
    cerr << "Looking for paths between " << site.start << " and " << site.end << endl;
#endif
    
    if(graph.paths.has_node_mapping(site.start.node) && graph.paths.has_node_mapping(site.end.node)) {
        // If we have some paths that visit both ends (in some orientation)
        
        // Get all the mappings to the end node, by path name
        auto& endmappings_by_name = graph.paths.get_node_mapping(site.end.node);
        
        for(auto& name_and_mappings : graph.paths.get_node_mapping(site.start.node)) {
            // Go through the paths that visit the start node
            
            // Grab their names
            auto& name = name_and_mappings.first;
            
            if(!endmappings_by_name.count(name_and_mappings.first)) {
                // No path by this name has any mappings to the end node. Skip
                // it early.
                continue;
            }
            
            for(auto* mapping : name_and_mappings.second) {
                // Start at each mapping in the appropriate orientation
                
#ifdef debug
#pragma omp critical (cerr)
                cerr << "Trying mapping of read/path " << name_and_mappings.first << endl;
#endif
                
                // How many times have we gone to the next mapping looking for a
                // mapping to the end node in the right orientation?
                size_t traversal_count = 0;
                
                // Do we want to go left (true) or right (false) from this
                // mapping? If start is a forward traversal and we found a
                // forward mapping, we go right. If either is backward we go
                // left, and if both are backward we go right again.
                bool traversal_direction = mapping->position().is_reverse() != site.start.backward;
                
                // What orientation would we want to find the end node in? If
                // we're traveling backward, we expect to find it in the
                // opposite direction to the one we were given.
                bool expected_end_orientation = site.end.backward != traversal_direction;
                
                // We're going to fill in this list with traversals.
                list<SiteTraversal::Visit> path_traversed;
                
                // And we're going to fill this with the sequence
                stringstream allele_stream;
                
                while(mapping != nullptr && traversal_count < max_path_search_steps) {
                    // Traverse along until we hit the end traversal or take too
                    // many steps
                    
#ifdef debug
#pragma omp critical (cerr)
                    cerr << "\tTraversing " << pb2json(*mapping) << endl;
#endif
                    
                    // Say we visit this node along the path, in this orientation
                    NodeTraversal node_traversal = NodeTraversal(graph.get_node(mapping->position().node_id()),
                                                                 mapping->position().is_reverse() != traversal_direction);
                    
                    // Stick the sequence of the node (appropriately oriented) in the stream for the allele sequence
                    string seq = node_traversal.node->sequence();
                    allele_stream << (node_traversal.backward ? reverse_complement(seq) : seq);
                    
                    if(node_traversal.node == site.end.node && node_traversal.backward == expected_end_orientation) {
                        // We have stumbled upon the end node in the orientation we wanted it in.
                        
                        if(results.count(allele_stream.str())) {
                            // It is already there! Increment the observation count.
#ifdef debug
#pragma omp critical (cerr)
                            cerr << "\tFinished; got known sequence " << allele_stream.str() << endl;
#endif
                            
                            if(reads_by_name.count(name)) {
                                // We are a read. Just increment count
                                results[allele_stream.str()].second++;
                            } else {
                                // We are a named path (like "ref")
                                if(results[allele_stream.str()].second < min_recurrence) {
                                    // Ensure that this allele doesn't get
                                    // eliminated, since ref or some other named
                                    // path supports it.
                                    results[allele_stream.str()].second = min_recurrence;
                                } else {
                                    results[allele_stream.str()].second++;
                                }
                            }
                        } else {
                            // Add it in. Give it a count of 1 if we are a read,
                            // and a count of min_recurrence (so it doesn't get
                            // filtered later) if we are a named non-read path
                            // (like "ref").
                            results[allele_stream.str()] = make_pair(path_traversed,
                                                                     reads_by_name.count(name) ? 1 : min_recurrence);
#ifdef debug
#pragma omp critical (cerr)
                            cerr << "\tFinished; got novel sequence " << allele_stream.str() << endl;
#endif
                        }
                        
//                        if(reads_by_name.count(name)) {
//                            // We want to log stats on reads that read all the
//                            // way through sites. But since we may be called
//                            // multiple times we need to send the unique read
//                            // name too.
//                            report_site_traversal(site, name);
//                        }
                        
                        // Then try the next embedded path
                        break;
                    }
                    
                    // We are not yet at the end of the of the site on this path
                    
                    // initialize visit with the current node traversal
                    SiteTraversal::Visit visit(node_traversal);
                    
                    // is this traversal at the start of a nested subsite?
                    Node* site_opposite_side = nullptr;
                    if (child_site_starts.count(node_traversal)) {
                        const NestedSite* child_site = child_site_starts[node_traversal];
                        site_opposite_side = child_site->end.node;
                        
                        visit.child = child_site;
                        
                        // add the site into the sequence since we are going to skip it
                        allele_stream << "(" << child_site->start.node->id() << ":" << child_site->end.node->id() << ")";
                        
                    }
                    else if (child_site_ends.count(node_traversal)) {
                        const NestedSite* child_site = child_site_starts[node_traversal];
                        site_opposite_side = child_site->start.node;
                        
                        visit.child = child_site;
                        visit.backward = true;
                        
                        // add the reverse site into the sequence since we are going to skip it
                        allele_stream << "(" << child_site->end.node->id() << ":" << child_site->start.node->id() << ")";
                    }
                    
                    path_traversed.push_back(visit);
                    
                    // Was this node traversal entering a nested subsite?
                    if (site_opposite_side) {
                        // Skip the site
                        if (traversal_direction) {
                            // Go backwards until you hit the other side of the site
                            while (mapping->position().node_id() != site_opposite_side->id()) {
                                mapping = graph.paths.traverse_left(mapping);
                                // Tick the counter so we don't go really far on long paths.
                                traversal_count++;
                            }
                        }
                        else {
                            // Go forwards until you hit the other side of the site
                            while (mapping->position().node_id() != site_opposite_side->id()) {
                                mapping = graph.paths.traverse_right(mapping);
                                // Tick the counter so we don't go really far on long paths.
                                traversal_count++;
                            }
                        }
                    }
                    else {
                        // Otherwise just move to the right (or left) one position
                        if(traversal_direction) {
                            // We're going backwards
                            mapping = graph.paths.traverse_left(mapping);
                        } else {
                            // We're going forwards
                            mapping = graph.paths.traverse_right(mapping);
                        }
                        // Tick the counter so we don't go really far on long paths.
                        traversal_count++;
                    }
                }
            }
        }
    }
    
    // Now collect the unique results
    vector<SiteTraversal> to_return;
    
    for(auto& result : results) {
        // Break out each result
        const string& seq = result.first;
        auto& visits = result.second.first;
        auto& count = result.second.second;
        
        if(count < min_recurrence) {
            // We don't have enough initial hits for this sequence to justify
            // trying to re-align the rest of the reads. Skip it. Note that the
            // reference path (and other named paths) will stuff in at least
            // min_recurrence to make sure we don't throw out their alleles.
            continue;
        }
        
        // Send out each list of visits
        to_return.emplace_back();
        to_return.back().visits = std::move(visits);
    }
    
    return to_return;
}
    
double FixedGenotypePriorCalculator::calculate_log_prior(const Genotype& genotype) {
    // Are all the alleles the same?
    bool all_same = true;
    // What is the common allele number (-1 for unset)
    int allele_value = -1;
    for(size_t i = 0; i < genotype.allele_size(); i++) {
        // For each allele in the genotype
        if(allele_value == -1) {
            // On the first one, grab it. Everyone else will have to match.
            allele_value = genotype.allele(i);
        }
        
        if(allele_value != genotype.allele(i)) {
            // There are two distinct allele values in here
            all_same = false;
            break;
        }
    }
    
    // Return the appropriate prior depending on whether the alleles are all the
    // same (homozygous) or not (heterozygous).
    return all_same ? homozygous_prior_ln : heterozygous_prior_ln;
}

TrivialTraversalFinder::TrivialTraversalFinder(VG& graph) : graph(graph) {
    // Nothing to do!
}

vector<SiteTraversal> TrivialTraversalFinder::find_traversals(const NestedSite& site) {
    // We'll fill this in and send it back
    vector<SiteTraversal> to_return;
    
    // We don't want to be duplicating partial paths, so we store for each
    // NodeTraversal we can reach the previous NodeTraversal we can reach it
    // from.
    map<NodeTraversal, NodeTraversal> previous;
    
    list<NodeTraversal> stack{site.start};
    
    while (!stack.empty()) { 
        // While there's still stuff on the stack
        
        // Grab the first thing
        NodeTraversal here = stack.front();
        stack.pop_front();
        
        if (here == site.end) {
            // Trace back a path
            SiteTraversal path;
            
            while (true) {
                // Until we get to the start of the site
            
                // Put this traversal on the front of the path
                path.visits.push_front(SiteTraversal::Visit(here));
                
                if (here == site.start) {
                    // Stop when we've reached the start of the site
                    break;
                }
                
                // Trace back
                here = previous.at(here);
            }
            
            // Stick the path on the back of the vector of paths
            to_return.emplace_back(std::move(path));
            
            // Stop eary after having found one path
            break;
        } else {
            // We haven't reached the end of the site
            
            for (NodeTraversal next : graph.nodes_next(here)) {
                // Look at all the places we can go from this node
                if (previous.count(next)) {
                    // We already know how to get there.
                    continue;
                }
                
                if (!site.nodes.count(next.node)) {
                    // We would be leaving the site, so we can't go there
                    continue;
                }
                
                // Remember how we got there
                previous[next] = here;
                // Explore it, depth first
                stack.push_front(next);
            }
        }
    }
    
    // When we get here, either we found a path, or there isn't one.
    return to_return;
}

}
