#ifndef DECON_HPP
#define DECON_HPP
#include <vector>
#include <set>
#include <array>
#include <list>
#include <string>
#include <iostream>
#include <unordered_map>
#include <map>
#include <climits>
#include <queue>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <stack>
#include "Variant.h"
#include "index.hpp"
#include "path.hpp"
#include "vg.hpp"
#include "vg.pb.h"
#include "Fasta.h"
#include "xg.hpp"
#include "position.hpp"
#include "vcfheader.hpp"
/** \file
* Deconstruct is getting rewritten.
* New functionality:
* -Detect superbubbles and bubbles
* -Fix command line interface.
* -harmonize on XG / raw graph (i.e. deprecate index)
* -Use unroll/DAGify if needed to avoid cycles

** Much of this is taken from Brankovic's
** "Linear-Time Superbubble Identification Algorithm for Genome Assembly"
*/
namespace vg{
    using namespace std;
    using namespace vcfh;
    struct SuperBubble{
      //A vector of topologically-sorted nodes in the superbubble.
      map<int, vector<id_t> > level_to_nodes;
      id_t start_node;
      id_t end_node;
      bool isNested;
    };

    class Deconstructor{
        public:

            Deconstructor();
            Deconstructor(VG* graph);
            ~Deconstructor();
            void set_xg(xg::XG* xindex);
            void unroll_my_vg(int steps);
            void dagify_my_vg(int steps);
            vg::VG* compact(int compact_steps);
            bool is_nested(SuperBubble sb);
            bool contains_nested(pair<int64_t, int64_t> start_and_end);
            SuperBubble report_superbubble(int64_t start, int64_t end);
            map<pair<id_t, id_t>, vector<id_t> > get_all_superbubbles();
            void sb2vcf( string outfile);
            

        private:

		  VG* my_vg;
          xg::XG* my_xg;
          map<pair<id_t, id_t>, vector<id_t> > my_sbs;
          map<id_t, pair<id_t, bool> > my_translation;
          map<id_t, pair<id_t, bool> > my_unroll_translation;
          map<id_t, pair<id_t, bool> > my_dagify_translation;
          map<id_t, SuperBubble> id_to_bub; 

          vector<id_t> reverse_topo_order;

          string mask_file = "";

          vector<SuperBubble> my_superbubbles;
          size_t my_max_length;
          size_t my_max_component_length;

		  vector<int64_t> nt_to_ids(deque<NodeTraversal>& nt);

          SuperBubble translate_id(id_t id);
		  void init();

    };
}
#endif
