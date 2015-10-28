#include "loki/service.h"
#include "loki/search.h"

#include <boost/property_tree/info_parser.hpp>

#include <valhalla/baldr/json.h>
#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/midgard/logging.h>

using namespace prime_server;
using namespace valhalla::baldr;

namespace {
  const headers_t::value_type CORS{"Access-Control-Allow-Origin", "*"};
  const headers_t::value_type JSON_MIME{"Content-type", "application/json;charset=utf-8"};
  const headers_t::value_type JS_MIME{"Content-type", "application/javascript;charset=utf-8"};

  //TODO: move json header to baldr
  //TODO: make objects serialize themselves

  json::ArrayPtr serialize_edges(const PathLocation& location, GraphReader& reader, bool verbose) {
    auto array = json::array({});
    std::unordered_multimap<uint64_t, PointLL> ids;
    for(const auto& edge : location.edges()) {
      try {
        //get the osm way id
        auto tile = reader.GetGraphTile(edge.id);
        auto* directed_edge = tile->directededge(edge.id);
        auto edge_info = tile->edgeinfo(directed_edge->edgeinfo_offset());
        //check if we did this one before
        auto range = ids.equal_range(edge_info->wayid());
        bool duplicate = false;
        for(auto id = range.first; id != range.second; ++id) {
          if(id->second == location.vertex()) {
            duplicate = true;
            break;
          }
        }
      }
      catch(...) {
        //this really shouldnt ever get hit
        LOG_WARN("Expected edge not found in graph but found by loki::search!");
      }
    }
    return array;
  }
}

namespace valhalla {
  namespace loki {

    worker_t::result_t loki_worker_t::matrix(const ACTION_TYPE& action, boost::property_tree::ptree& request) {
      //see if any locations pairs are unreachable or too far apart
      auto lowest_level = reader.GetTileHierarchy().levels().rbegin();
      for(auto location = ++locations.cbegin(); location != locations.cend(); ++location) {
        //check connectivity
        uint32_t a_id = lowest_level->second.tiles.TileId(std::prev(location)->latlng_);
        uint32_t b_id = lowest_level->second.tiles.TileId(location->latlng_);
        if(!reader.AreConnected({a_id, lowest_level->first, 0}, {b_id, lowest_level->first, 0}))
          throw std::runtime_error("Locations are in unconnected regions. Go check/edit the map at osm.org");
      }

      auto matrix_type = "";
      auto max_distance = 200000.0;
      switch (action) {
       case ONE_TO_MANY:
         matrix_type = "one_to_many";
         max_distance = config.get<float>("service_limits." + std::string(matrix_type) + ".max_distance");
         check_max_distance(0,0,0,locations.size(),max_distance);
         break;
       case MANY_TO_ONE:
         matrix_type = "many_to_one";
         max_distance = config.get<float>("service_limits." + std::string(matrix_type) + ".max_distance");
         for(size_t i = 0; i < locations.size(); ++i)
           check_max_distance(i,locations.size() - 1, i, i + 1,max_distance);
         break;
       case MANY_TO_MANY:
         matrix_type = "many_to_many";
         max_distance = config.get<float>("service_limits." + std::string(matrix_type) + ".max_distance");
         for(size_t i = 0; i < locations.size(); ++i)
            check_max_distance(i, 0, locations.size() * i, locations.size() * (i + 1),max_distance);
         break;
     }

      auto max_locations = config.get<size_t>("service_limits." + std::string(matrix_type) + ".max_locations");
      //check that location size does not exceed max.
      if (locations.size() > max_locations)
        throw std::runtime_error("Number of locations exceeds the max location limit.");
      LOG_INFO("Location size::" + std::to_string(locations.size()));

      //correlate the various locations to the underlying graph
      for(size_t i = 0; i < locations.size(); ++i) {
        auto correlated = loki::Search(locations[i], reader, costing_filter);
        request.put_child("correlated_" + std::to_string(i), correlated.ToPtree(i));
      }
      request.put("matrix_type", std::string(matrix_type));

      std::stringstream stream;
      boost::property_tree::write_info(stream, request);
      worker_t::result_t result{true};
      result.messages.emplace_back(stream.str());
      return result;
    }

    void loki_worker_t::check_max_distance(const size_t origin, const size_t destination, const size_t start, const size_t end, float max_distance) {
      //one to many should be distance between:a,b a,c ; many to one: a,c b,c ; many to many should be all pairs
      for(size_t i = start; i < end; ++i) {
        //check if distance between latlngs exceed max distance limit the chosen matrix type
        auto path_distance = locations[origin].latlng_.Distance(locations[destination + (i - start)].latlng_);
        max_distance-=path_distance;

        if (max_distance < 0)
          throw std::runtime_error("The path distance of " + std::to_string(path_distance) + " exceeds the max distance limit by " + std::to_string(max_distance) + ".");
      }
    }
  }
}
