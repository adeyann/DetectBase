#include "ServiceBlockProfile.h"
#include "MgenLogger.h"

#include <queue>
#include <sstream>

namespace MGEN
{
    static inline std::string ToString( ServiceStreamType type )
    {
        switch (type)
        {
        case ServiceStreamType::None:             return "None";
        case ServiceStreamType::InternalQueueing: return "InternalQueueing";
        case ServiceStreamType::SocketIO:         return "SocketIO";
        case ServiceStreamType::RTSP:             return "RTSP";
        case ServiceStreamType::GRPC:             return "GRPC";
        default:                                  return "Unknown";
        }
    }

    inline std::string ToString(ServiceBlockModuleType type)
    {
        switch (type)
        {
        case ServiceBlockModuleType::DETECTOR_RTSP:          return "DETECTOR_RTSP";
        case ServiceBlockModuleType::DETECTOR_SocketIO:      return "DETECTOR_SocketIO";
        default:                                             return "Unknown";
        }
    }

    ServiceBlockProfile::ServiceBlockProfile( int block_id, ServiceBlockModuleType module_type, ServiceBlockBuildType build_type )
        : service_block_id_ ( block_id )
        , module_type_      ( module_type )
        , build_type_       ( build_type )
    {
        //
    }

    int ServiceBlockProfile::GetCurrID( void ) const noexcept
    {
        return service_block_id_;
    }

    ServiceBlockModuleType ServiceBlockProfile::GetServiceBlockModuleType( void ) const noexcept
    {
        return module_type_;
    }

    ServiceBlockBuildType ServiceBlockProfile::GetServiceBlockBuildType( void ) const noexcept
    {
        return build_type_;
    }

    void ServiceBlockProfile::AddInput( int from_id, ServiceStreamType stream_type )
    {
        input_sources_[from_id] = stream_type;
    }

    void ServiceBlockProfile::RemoveInput( int from_id )
    {
        input_sources_.erase(from_id);
    }

    const std::unordered_map<int, ServiceStreamType>& ServiceBlockProfile::GetInputs( void ) const noexcept
    {
        return input_sources_;
    }

    std::optional<ServiceStreamType> ServiceBlockProfile::GetInputStreamType( int from_id ) const
    {
        auto it = input_sources_.find(from_id);
        if (it != input_sources_.end())
            return it->second;
        return std::nullopt;
    }

    bool ServiceBlockProfile::HasInput( int from_id ) const
    {
        return input_sources_.count( from_id ) > 0;
    }

    void ServiceBlockProfile::AddOutput( int to_id, ServiceStreamType stream_type )
    {
        output_targets_[to_id] = stream_type;
    }

    void ServiceBlockProfile::RemoveOutput( int to_id )
    {
        output_targets_.erase( to_id );
    }

    const std::unordered_map<int, ServiceStreamType>& ServiceBlockProfile::GetOutputs( void ) const noexcept
    {
        return output_targets_;
    }

    std::optional<ServiceStreamType> ServiceBlockProfile::GetOutputStreamType( int to_id ) const
    {
        auto it = output_targets_.find(to_id);
        if (it != output_targets_.end())
            return it->second;
        return std::nullopt;
    }

    bool ServiceBlockProfile::HasOutput( int to_id ) const
    {
        return output_targets_.count( to_id ) > 0;
    }

    bool ServiceBlockProfile::IsSource( void ) const
    {
        // 외부 입력만 존재하거나 아무 입력도 없는 경우 → source
        if( input_sources_.empty() )
            return true;

        for( const auto& [from_id, _] : input_sources_ ){
            if( from_id != SERVICE_BLOCK_ID_EXTERN_SERV )
                return false; // 내부 입력이 하나라도 있으면 source 아님
        }
        return true; // 외부 입력만 있다면 source
    }

    bool ServiceBlockProfile::IsSink( void ) const
    {
        return output_targets_.empty();
    }

    bool EntireServiceBlockProfileGraph::AddServiceBlock( int id, ServiceBlockModuleType module_type, ServiceBlockBuildType build_type )
    {
        if( blocks_.count(id) > 0 )
            return false;

        blocks_.emplace( id, ServiceBlockProfile( id, module_type, build_type ) );
        return true;
    }

    bool EntireServiceBlockProfileGraph::AddStreamPipeline( int from_id, int to_id, ServiceStreamType stream_type )
    {
        if( !HasBlock(from_id) || !HasBlock(to_id) )
            return false;

        blocks_.at(to_id).AddInput(from_id, stream_type);
        blocks_.at(from_id).AddOutput(to_id, stream_type);
        reverse_input_map_[to_id].insert(from_id);

        return true;
    }

    bool EntireServiceBlockProfileGraph::AddInputFromExtern( int to_id, ServiceStreamType stream_type )
    {
        // 외부 입력 스트림도 실제 to 블록에 입력으로 추가해야 DOT 시각화에 반영된다
        if( !HasBlock(to_id) )
            return false;

        blocks_.at(to_id).AddInput(SERVICE_BLOCK_ID_EXTERN_SERV, stream_type);
        reverse_input_map_[to_id].insert(SERVICE_BLOCK_ID_EXTERN_SERV);

        return true;
    }

    bool EntireServiceBlockProfileGraph::AddOutputToExtern( int from_id, ServiceStreamType stream_type )
    {
        if( !HasBlock(from_id) )
            return false;

        extern_streams_[from_id] = stream_type;
        return true;
    }

    bool EntireServiceBlockProfileGraph::HasBlock( int id ) const
    {
        return blocks_.count(id) > 0;
    }

    std::optional<ServiceBlockProfile> EntireServiceBlockProfileGraph::GetBlockProfile( int id ) const
    {
        auto it = blocks_.find(id);
        if( it != blocks_.end() )
            return it->second;
        return std::nullopt;
    }

    std::optional<ServiceBlockProfile> EntireServiceBlockProfileGraph::GetBlockProfile( ServiceBlockModuleType type ) const
    {
        // NEW-11: id 사용 안 됨 → throwaway 로 변경
        for( const auto& [_, profile] : blocks_ ) {
            if( profile.GetServiceBlockModuleType() == type )
                return profile;
        }
        return std::nullopt;
    }

    std::vector<int> EntireServiceBlockProfileGraph::GetAllBlockIDs( void ) const
    {
        std::vector<int> ids;
        for( const auto& [id, _] : blocks_ )
            ids.push_back(id);
        return ids;
    }

    std::set<ServiceStreamType> EntireServiceBlockProfileGraph::GetExternServiceStreamList() const
    {
        std::set<ServiceStreamType> list;
        // NEW-11: id 사용 안 됨 → throwaway 로 변경
        for( const auto& [_, type] : extern_streams_ )
            list.insert(type);
        return list;
    }

    std::vector<int> EntireServiceBlockProfileGraph::GetExecutionOrder() const
    {
        std::unordered_map<int, int> in_degree;
        for (const auto& [id, _] : blocks_)
            in_degree[id] = 0;

        for (const auto& [id, profile] : blocks_)
        {
            for (const auto& [to_id, _] : profile.GetOutputs())
                in_degree[to_id]++;
        }

        std::queue<int> q;
        for (const auto& [id, deg] : in_degree)
            if (deg == 0)
                q.push(id);

        std::vector<int> order;
        while (!q.empty())
        {
            int curr = q.front(); q.pop();
            order.push_back(curr);

            for (const auto& [next_id, _] : blocks_.at(curr).GetOutputs())
            {
                if (--in_degree[next_id] == 0)
                    q.push(next_id);
            }
        }

        return order;
    }

    bool EntireServiceBlockProfileGraph::DetectCycle() const
    {
        return GetExecutionOrder().size() != blocks_.size();
    }

    std::string EntireServiceBlockProfileGraph::ToDOTGraph(const std::string& graph_name) const
    {
        std::ostringstream oss;
        oss << "digraph \"" << graph_name << "\" {\n";

        // [1] 블록 노드 정의
        for (const auto& [id, profile] : blocks_) {
            std::string label = ToString(profile.GetServiceBlockModuleType()) + " (" + std::to_string(id) + ")";
            oss << "  " << id << " [label=\"" << label << "\"];\n";
        }

        // [2] 스트림 파이프라인 연결
        std::set<std::string> extern_inputs_rendered;
        for (const auto& [to_id, profile] : blocks_) {
            for (const auto& [from_id, stream] : profile.GetInputs()) {
                std::string stream_label = ToString(stream);
                if (from_id == SERVICE_BLOCK_ID_EXTERN_SERV) {
                    std::string extern_input_id = "extern_input_" + stream_label;
                    if (extern_inputs_rendered.insert(extern_input_id).second) {
                        oss << "  " << extern_input_id << " [shape=ellipse, style=dashed, label=\"Extern Input: " << stream_label << "\"];\n";
                    }
                    oss << "  " << extern_input_id << " -> " << to_id << " [label=\"Stream: " << stream_label << "\"];\n";
                } else {
                    oss << "  " << from_id << " -> " << to_id << " [label=\"Stream: " << stream_label << "\"];\n";
                }
            }
        }

        // [3] 외부 출력 정의
        for (const auto& [from_id, stream] : extern_streams_) {
            std::string stream_label = ToString(stream);
            std::string extern_node_id = "extern_output_" + stream_label;
            oss << "  " << extern_node_id << " [shape=box, style=dashed, label=\"Extern Output: " << stream_label << "\"];\n";
            oss << "  " << from_id << " -> " << extern_node_id << " [label=\"Stream: " << stream_label << "\"];\n";
        }

        oss << "}\n";
        return oss.str();
    }

} // namespace MGEN
