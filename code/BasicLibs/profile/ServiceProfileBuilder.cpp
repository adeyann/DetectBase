#include "ServiceProfileBuilder.h"
#include "ServiceStreamTypes.h"
#include "MgenLogger.h"

#include <set>

namespace MGEN
{
    ServiceProfile ServiceProfileBuilder::Build( void )
    {
        static int unique_block_id { SERVICE_BLOCK_ID_BOOT_LOADER };

        EntireServiceBlockProfileGraph graph;

        bool print_service_dag_graph = false;

        // DETECTOR 단일 분기
        // [1] RTSP 블록
        int rtsp_id = unique_block_id++;
        graph.AddServiceBlock   ( rtsp_id, ServiceBlockModuleType::DETECTOR_RTSP, ServiceBlockBuildType::ByUnit ); // CREATE
        graph.AddInputFromExtern( rtsp_id, ServiceStreamType::RTSP ); // IN
        graph.AddOutputToExtern ( rtsp_id, ServiceStreamType::RTSP ); // OUT

        // [2] SocketIO 블록 (RTSP → SocketIO)
        int sio_id = unique_block_id++;
        graph.AddServiceBlock   ( sio_id, ServiceBlockModuleType::DETECTOR_SocketIO, ServiceBlockBuildType::Unique ); // CREATE
        graph.AddInputFromExtern( sio_id, ServiceStreamType::SocketIO );                  // IN
        graph.AddStreamPipeline ( rtsp_id, sio_id, ServiceStreamType::InternalQueueing ); // IN
        graph.AddOutputToExtern ( sio_id, ServiceStreamType::SocketIO );                  // OUT

        if( ServiceGraphValidator::Validate( graph, EntireServiceTag::DETECTOR, print_service_dag_graph ) == false ){
            exit(-1);
        }

        return ServiceProfile( EntireServiceTag::DETECTOR, graph );
    }

    bool ServiceGraphValidator::Validate( const EntireServiceBlockProfileGraph& graph, std::string_view tag, bool log_dot )
    {
        bool is_valid = true;

        if( graph.DetectCycle() ){
            MLOG_ERROR("    - [%s] Graph contains a cycle.", std::string(tag).c_str());
            is_valid = false;
        }

        const auto exec_order = graph.GetExecutionOrder();
        const auto all_ids    = graph.GetAllBlockIDs();

        if( exec_order.size() != all_ids.size() ){
            MLOG_ERROR("    - [%s] Topological sort mismatch. (%zu != %zu)", std::string(tag).c_str(), exec_order.size(), all_ids.size());
            is_valid = false;
        }

        if( graph.GetExternServiceStreamList().empty() ){
            MLOG_WARN("    - [%s] No external output stream defined.", std::string(tag).c_str());
        }

        bool has_source = false;
        for( int id : all_ids ){
            const auto block = graph.GetBlockProfile(id);
            if( block.has_value() && block->IsSource() ){
                has_source = true;
                break;
            }
        }
        if( !has_source ){
            MLOG_ERROR("    - [%s] No source block defined (no input connections).", std::string(tag).c_str());
            is_valid = false;
        }

        if( !is_valid ){
            MLOG_ERROR("    - DAG [%s] validation failed.", std::string(tag).c_str());
        } else {
            // MLOG_INFO("    - DAG Validation : Success [%s]", std::string(tag).c_str());
        }

        if( log_dot ){
            MLOG_INFO("DOT graph for [%s]:\n\n%s", std::string(tag).c_str(), graph.ToDOTGraph().c_str());
        }

        return is_valid;
    }

} // namespace MGEN
