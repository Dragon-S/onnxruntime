#pragma once

#include "DmlGraphFusionHelper.h"


namespace Dml
{
namespace DmlGraphFusionHelper
{
    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        ORT_THROW_IF_FAILED(provider->UploadToResource(buffer.Get(), tensorPtr, tensorByteSize));

        return buffer;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateCpuResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        // Map the buffer and copy the data
        void* bufferData = nullptr;
        D3D12_RANGE range = {0, tensorByteSize};
        ORT_THROW_IF_FAILED(buffer->Map(0, &range, &bufferData));
        memcpy(bufferData, tensorPtr, tensorByteSize);
        buffer->Unmap(0, &range);

        return buffer;
    }

    bool GetGraphInputConstness(
        uint32_t index,
        const gsl::span<const std::string> fusedNodeInputArgOriginalNames,
        const std::unordered_map<std::string, onnx::TensorProto>& transferredInitializerMap) 
    {
        // Transferred initializers are uploaded to GPU memory
        auto iter = transferredInitializerMap.find(fusedNodeInputArgOriginalNames[index]);
        if (iter != transferredInitializerMap.end())
        {
            return true;
        }

        return false;
    }

    void UnwrapTensor(
        Windows::AI::MachineLearning::Adapter::IWinmlExecutionProvider* winmlProvider,
        const onnxruntime::Tensor* tensor,
        ID3D12Resource** resource,
        uint64_t* allocId) 
    {
        IUnknown* allocationUnk = static_cast<IUnknown*>(const_cast<void*>(tensor->DataRaw()));
        Microsoft::WRL::ComPtr<IUnknown> resourceUnk;
        winmlProvider->GetABIDataInterface(false, allocationUnk, &resourceUnk);

        *allocId = winmlProvider->TryGetPooledAllocationId(allocationUnk, 0);

        ORT_THROW_IF_FAILED(resourceUnk->QueryInterface(resource));
    }

    void ProcessInputData(
        const ExecutionProviderImpl* providerImpl,
        const std::vector<uint8_t>& inputsConstant,
        std::vector<DML_INPUT_GRAPH_EDGE_DESC>& inputEdges,
        const gsl::span<const std::string> subGraphInputArgNames,
        _Out_ std::vector<bool>& inputsUsed,
        _Inout_ std::vector<DML_BUFFER_BINDING>& initInputBindings,
        _Inout_ std::vector<ComPtr<ID3D12Resource>>& initInputResources,
        _Inout_ std::vector<ComPtr<ID3D12Resource>>& nonOwnedGraphInputsFromInitializers,
        _Inout_ std::vector<ComPtr<ID3D12Resource>>& initializeResourceRefs,
        _Inout_opt_ std::vector<std::vector<std::byte>>* inputRawData,
        _Inout_ std::unordered_map<std::string, onnx::TensorProto>& transferredInitializerMap)
    {
        
        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(subGraphInputArgNames.size());
        
        // Determine the last input which uses an initializer, so initializers can be freed incrementally
        // while processing each input in order.
        std::map<const onnx::TensorProto*, uint32_t> initializerToLastInputIndexMap;
        for (uint32_t i = 0; i < fusedNodeInputCount; i++) 
        {
            auto iter = transferredInitializerMap.find(subGraphInputArgNames[i]);
            if (iter != transferredInitializerMap.end()) {
                initializerToLastInputIndexMap[&iter->second] = i;
            }
        }

        // Walk through each graph edge and mark used inputs
        inputsUsed.assign(fusedNodeInputCount, false);
        for (const DML_INPUT_GRAPH_EDGE_DESC& edge : inputEdges) {
            inputsUsed[edge.GraphInputIndex] = true;
        }

        for (uint32_t i = 0; i < initInputBindings.size(); i++)
        {
            // If the input isn't actually used by the graph, nothing ever needs to be bound (either for
            // initialization or execution). So just throw away the transferred initializer and skip this input.
            if (!inputsUsed[i])
            {
                transferredInitializerMap.erase(subGraphInputArgNames[i]);

                if (inputRawData)
                {
                    inputRawData->push_back(std::vector<std::byte>());
                }

                continue;
            }

            // Look for the initializer among those transferred from the graph during partitioning
            auto iter = transferredInitializerMap.find(subGraphInputArgNames[i]);
            if (iter != transferredInitializerMap.end())
            {
                std::byte* tensorPtr = nullptr;
                size_t tensorByteSize = 0;
                std::unique_ptr<std::byte[]> unpackedTensor;

                auto& initializer = iter->second;

                // The tensor may be stored as raw data or in typed fields.
                if (initializer.has_raw_data())
                {
                    tensorPtr = (std::byte*)(initializer.raw_data().c_str());
                    tensorByteSize = initializer.raw_data().size();
                }
                else
                {
                    std::tie(unpackedTensor, tensorByteSize) = Windows::AI::MachineLearning::Adapter::UnpackTensor(initializer);
                    tensorPtr = unpackedTensor.get(); 
                }

                // Tensor sizes in DML must be a multiple of 4 bytes large.
                tensorByteSize = AlignToPow2<size_t>(tensorByteSize, 4);

                if (inputRawData)
                {
                    inputRawData->push_back(std::vector<std::byte>(tensorPtr, tensorPtr + tensorByteSize));
                }

                if (!inputsConstant[i])
                {
                    // Store the resource to use during execution
                    ComPtr<ID3D12Resource> defaultBuffer = CreateResource(providerImpl, tensorPtr, tensorByteSize);
                    nonOwnedGraphInputsFromInitializers[i] = defaultBuffer;
                    initializeResourceRefs.push_back(std::move(defaultBuffer));
                }
                else
                {
                    ComPtr<ID3D12Resource> initializeInputBuffer;

                    // D3D_FEATURE_LEVEL_1_0_CORE doesn't support Custom heaps
                    if (providerImpl->IsMcdmDevice())
                    {
                        initializeInputBuffer = CreateResource(providerImpl, tensorPtr, tensorByteSize);
                    }
                    else
                    {
                        initializeInputBuffer = CreateCpuResource(providerImpl, tensorPtr, tensorByteSize);
                    }

                    // Set the binding for operator initialization to the buffer
                    initInputBindings[i].Buffer = initializeInputBuffer.Get();
                    initInputBindings[i].SizeInBytes = tensorByteSize;
                    initializeResourceRefs.push_back(std::move(initializeInputBuffer));
                }

                // Free the initializer if this is the last usage of it.
                if (initializerToLastInputIndexMap[&initializer] == i)
                {
                    transferredInitializerMap.erase(iter);
                }
            }
            else if (inputRawData)
            {
                inputRawData->push_back(std::vector<std::byte>());
            }
        }

        // All initializers should have been consumed and freed above
        assert(transferredInitializerMap.empty());
    }

    std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>>
    GetInitializerToPartitionMap(
        const onnxruntime::GraphViewer& graph,
        gsl::span<std::unique_ptr<GraphPartition>> partitions
    )
    {
        std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>> initializerPartitionMap;
        for (uint32_t partitionIndex = 0; partitionIndex < gsl::narrow_cast<uint32_t>(partitions.size()); ++partitionIndex)
        {
            auto& partition = partitions[partitionIndex];

            // Skip partitions which have been merged into other partitions
            if (partition->GetRootMergedPartition() != partition.get())
            {
                continue;
            }

            for (const std::string& input : partition->GetInputs())
            {
                const onnx::TensorProto* tensor = nullptr;
                if (graph.GetInitializedTensor(input, tensor))
                {
                    initializerPartitionMap[tensor].push_back(partitionIndex);
                }
            }
        }

        return initializerPartitionMap;
    }

    void ConvertGraphDesc(
        const Dml::GraphDescBuilder::GraphDesc& graphDesc,
        _Out_ DML_GRAPH_DESC& dmlGraphDesc,
        const uint32_t inputCount,
        const uint32_t outputCount,
        _Inout_ std::vector<DML_OPERATOR_GRAPH_NODE_DESC>& dmlOperatorGraphNodes,
        _Inout_ std::vector<DML_GRAPH_NODE_DESC>& dmlGraphNodes,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlInputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlOutputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlIntermediateEdges)
    {
        for (size_t i = 0; i < graphDesc.nodes.size(); ++i)
        {
            dmlOperatorGraphNodes[i] = DML_OPERATOR_GRAPH_NODE_DESC{graphDesc.nodes[i].op.Get()};
            dmlGraphNodes[i] = DML_GRAPH_NODE_DESC{DML_GRAPH_NODE_TYPE_OPERATOR, &dmlOperatorGraphNodes[i]};
        }

        for (size_t i = 0; i < graphDesc.inputEdges.size(); ++i)
        {
            dmlInputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INPUT, &graphDesc.inputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.outputEdges.size(); ++i)
        {
            dmlOutputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_OUTPUT, &graphDesc.outputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.intermediateEdges.size(); ++i)
        {
            dmlIntermediateEdges[i] =
                DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &graphDesc.intermediateEdges[i]};
        }

        dmlGraphDesc.InputCount = inputCount;
        dmlGraphDesc.OutputCount = outputCount;
        dmlGraphDesc.NodeCount = gsl::narrow_cast<uint32_t>(dmlGraphNodes.size());
        dmlGraphDesc.Nodes = dmlGraphNodes.data();
        dmlGraphDesc.InputEdgeCount = gsl::narrow_cast<uint32_t>(dmlInputEdges.size());
        dmlGraphDesc.InputEdges = dmlInputEdges.data();
        dmlGraphDesc.OutputEdgeCount = gsl::narrow_cast<uint32_t>(dmlOutputEdges.size());
        dmlGraphDesc.OutputEdges = dmlOutputEdges.data();
        dmlGraphDesc.IntermediateEdgeCount = gsl::narrow_cast<uint32_t>(dmlIntermediateEdges.size());
        dmlGraphDesc.IntermediateEdges = dmlIntermediateEdges.data();
    }

    void CreateIDmlCompiledOperatorAndRegisterKernel(
        onnxruntime::Graph& graph, 
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        const onnxruntime::Node& fusedNode,
        const std::unordered_map<std::string, GraphNodeProperties>& partitionNodePropsMap,
        std::shared_ptr<std::unordered_map<std::string, onnx::TensorProto>> transferredInitializerMap,
        const ExecutionProviderImpl* providerImpl,
        onnxruntime::KernelRegistry* registryForPartitionKernels)
    {
        // convert partitionONNXGraph into DML EP GraphDesc
        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->inputs.size());
        const uint32_t fusedNodeOutputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->outputs.size());

        std::vector<uint8_t> inputsConstant(fusedNodeInputCount);
        for (uint32_t index = 0; index < fusedNodeInputCount; ++index)
        {
            inputsConstant[index] = GetGraphInputConstness(index, indexedSubGraph.GetMetaDef()->inputs, *transferredInitializerMap);
        }

        ComPtr<IDMLDevice> device;
        ORT_THROW_IF_FAILED(providerImpl->GetDmlDevice(device.GetAddressOf()));
        GraphDescBuilder::GraphDesc graphDesc = GraphDescBuilder::BuildGraphDesc(
            inputsConstant.data(),
            inputsConstant.size(),
            *transferredInitializerMap,
            graph,
            indexedSubGraph,
            partitionNodePropsMap,
            device.Get(),
            providerImpl);

        // convert DML EP GraphDesc into DML_GRAPH_DESC and create IDMLCompiledOperator
        DML_GRAPH_DESC dmlGraphDesc = {};
        std::vector<DML_OPERATOR_GRAPH_NODE_DESC> dmlOperatorGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_NODE_DESC> dmlGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlInputEdges(graphDesc.inputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlOutputEdges(graphDesc.outputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlIntermediateEdges(graphDesc.intermediateEdges.size());
        ConvertGraphDesc(
            graphDesc, 
            dmlGraphDesc, 
            fusedNodeInputCount,
            fusedNodeOutputCount,
            dmlOperatorGraphNodes,
            dmlGraphNodes,
            dmlInputEdges,
            dmlOutputEdges,
            dmlIntermediateEdges);

        DML_EXECUTION_FLAGS executionFlags = DML_EXECUTION_FLAG_NONE;
        if (graphDesc.reuseCommandList)
        {
            executionFlags |= DML_EXECUTION_FLAG_DESCRIPTORS_VOLATILE;
        }

        // Query DML execution provider to see if metacommands is enabled
        if (!providerImpl->MetacommandsEnabled())
        {
            executionFlags |= DML_EXECUTION_FLAG_DISABLE_META_COMMANDS;
        }

        ComPtr<IDMLDevice1> device1;
        ORT_THROW_IF_FAILED(device.As(&device1));
        ComPtr<IDMLCompiledOperator> compiledExecutionPlanOperator;
        ORT_THROW_IF_FAILED(device1->CompileGraph(
            &dmlGraphDesc,
            executionFlags,
            IID_PPV_ARGS(&compiledExecutionPlanOperator)));

        // Populate input bindings for operator initialization
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> initializeResourceRefs; // For lifetime control
        std::vector<DML_BUFFER_BINDING> initInputBindings(fusedNodeInputCount);
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> initInputResources;  // For lifetime control
        std::vector<ComPtr<ID3D12Resource>> nonOwnedGraphInputsFromInitializers(fusedNodeInputCount);
        
        std::vector<bool> inputsUsed;
        ProcessInputData(
            providerImpl,
            inputsConstant,
            graphDesc.inputEdges,
            indexedSubGraph.GetMetaDef()->inputs,
            inputsUsed,
            initInputBindings,
            initInputResources,
            nonOwnedGraphInputsFromInitializers,
            initializeResourceRefs,
            nullptr,
            *transferredInitializerMap);
            
        // Allocate a persistent resource and initialize the operator
        ComPtr<ID3D12Resource> persistentResource;
        ComPtr<IUnknown> persistentResourceAllocatorUnk; // Controls when the persistent resource is returned to the allocator
        std::optional<DML_BUFFER_BINDING> persistentResourceBinding;
        UINT64 persistentResourceSize = compiledExecutionPlanOperator->GetBindingProperties().PersistentResourceSize;
        if (persistentResourceSize > 0)
        {
            ORT_THROW_IF_FAILED(providerImpl->AllocatePooledResource(
                static_cast<size_t>(persistentResourceSize),
                AllocatorRoundingMode::Disabled,
                persistentResource.GetAddressOf(),
                persistentResourceAllocatorUnk.GetAddressOf()));

            persistentResourceBinding = DML_BUFFER_BINDING { persistentResource.Get(), 0, persistentResourceSize };
        }

        ORT_THROW_IF_FAILED(providerImpl->InitializeOperator(
            compiledExecutionPlanOperator.Get(),
            persistentResourceBinding ? &*persistentResourceBinding : nullptr,
            gsl::make_span(initInputBindings)));


        // lamda captures for the kernel registration
        // m_inputUsed
        Windows::AI::MachineLearning::Adapter::EdgeShapes outputShapes;
        ORT_THROW_HR_IF(E_UNEXPECTED, !TryGetStaticOutputShapes(fusedNode, outputShapes));
        std::vector<DML_INPUT_GRAPH_EDGE_DESC>& inputEdges = graphDesc.inputEdges;
        bool resuableCommandList = graphDesc.reuseCommandList;
        auto fused_kernel_func = [compiledExecutionPlanOperator,
                                  outputShapes,
                                  resuableCommandList,
                                  nonOwnedGraphInputsFromInitializers,
                                  initializeResourceRefs,
                                  inputsConstant,
                                  inputsUsed,
                                  persistentResource,
                                  persistentResourceAllocatorUnk,
                                  persistentResourceBinding]
                    (onnxruntime::FuncManager& func_mgr, const onnxruntime::OpKernelInfo& info, std::unique_ptr<onnxruntime::OpKernel>& out) mutable ->onnxruntime::Status
        {
            out.reset(CreateFusedGraphKernel(info, 
                                             compiledExecutionPlanOperator,
                                             outputShapes,
                                             resuableCommandList,
                                             nonOwnedGraphInputsFromInitializers,
                                             initializeResourceRefs,
                                             inputsConstant,
                                             inputsUsed,
                                             persistentResource,
                                             persistentResourceAllocatorUnk,
                                             persistentResourceBinding));
            return Status::OK();
        };

        // build the kernel definition on the fly, and register it to the fused_kernel_regisitry.
        onnxruntime::KernelDefBuilder builder;
        builder.SetName(indexedSubGraph.GetMetaDef()->name)
            .SetDomain(indexedSubGraph.GetMetaDef()->domain)
            .SinceVersion(indexedSubGraph.GetMetaDef()->since_version)
            .Provider(onnxruntime::kDmlExecutionProvider);
        ORT_THROW_IF_ERROR(registryForPartitionKernels->Register(builder, fused_kernel_func));
    }

    void FusePartitionAndRegisterKernel(
        GraphPartition* partition,
        uint32_t partitionIndex,
        onnxruntime::Graph& graph,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties>& graphNodePropertyMap,
        onnxruntime::KernelRegistry* registryForPartitionKernels,
        const std::string& partitionKernelPrefix,
        std::shared_ptr<std::unordered_map<std::string, onnx::TensorProto>> transferredInitializerMap,
        const ExecutionProviderImpl* providerImpl)
    {
        assert(partition->IsDmlGraphPartition());

        onnxruntime::IndexedSubGraph indexedSubGraph;
        // Create a definition for the node.  The name must be unique.
        auto def = std::make_unique<onnxruntime::IndexedSubGraph::MetaDef>();
        def->name = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_NAME_PREFIX + partitionKernelPrefix + std::to_string(partitionIndex);
        def->domain = DmlGraphFusionTransformer::DML_GRAPH_FUSION_NODE_DOMAIN;
        def->since_version = 1;
        def->inputs.insert(def->inputs.begin(), partition->GetInputs().begin(), partition->GetInputs().end());
        def->outputs.insert(def->outputs.begin(), partition->GetOutputs().begin(), partition->GetOutputs().end());

        indexedSubGraph.SetMetaDef(std::move(def));
        indexedSubGraph.nodes = std::move(partition->GetNodeIndices());
        auto& fusedNode = graph.BeginFuseSubGraph(indexedSubGraph, indexedSubGraph.GetMetaDef()->name);
        fusedNode.SetExecutionProviderType(onnxruntime::kDmlExecutionProvider);
        
        // Populate properties which will be passed to OpKernel for this graph via the function below
        std::unordered_map<std::string, GraphNodeProperties> partitionNodePropsMap;
        for (auto nodeIndex : indexedSubGraph.nodes)
        {
            const onnxruntime::Node* node = graph.GetNode(nodeIndex);

#ifdef PRINT_PARTITON_INFO
            printf("Partition %u\t%s\n", partitionIndex, GraphDescBuilder::GetUniqueNodeName(*node).c_str());
#endif
            partitionNodePropsMap.insert(std::make_pair(
                GraphDescBuilder::GetUniqueNodeName(*node), std::move(graphNodePropertyMap[node])));
        }

#ifdef PRINT_PARTITON_INFO
        printf("\n");
#endif
        CreateIDmlCompiledOperatorAndRegisterKernel(
            graph, 
            indexedSubGraph,
            fusedNode,
            partitionNodePropsMap, 
            transferredInitializerMap, 
            providerImpl,
            registryForPartitionKernels);
        graph.FinalizeFuseSubGraph(indexedSubGraph, fusedNode);
    }
}
}