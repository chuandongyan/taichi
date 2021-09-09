#include "taichi/ir/ir.h"
#include "taichi/ir/statements.h"
#include "taichi/ir/transforms.h"
#include "taichi/ir/analysis.h"
#include "taichi/transforms/make_mesh_index_mapping_local.h"

namespace taichi {
namespace lang {

const PassID MakeMeshIndexMappingLocal::id = "MakeMeshIndexMappingLocal";

namespace irpass {

void make_mesh_index_mapping_local_offload(OffloadedStmt *offload,
                                           const CompileConfig &config,
                                           const std::string &kernel_name) {
  if (offload->task_type != OffloadedStmt::TaskType::mesh_for) {
    return;
  }

  // mesh_for should skip make_block_local pass
  std::set<std::pair<mesh::MeshElementType, mesh::ConvType>> mappings;

  // TODO(changyu): A analyzer to determinte which mapping should be localized
  mappings.insert(std::make_pair(mesh::MeshElementType::Vertex,
                                 mesh::ConvType::l2g));  // FIXME: A hack

  std::size_t bls_offset_in_bytes = 0;
  auto &block = offload->bls_prologue;

  for (auto [element_type, conv_type] : mappings) {
    // There is not corresponding mesh element attribute read/write,
    // It's useless to localize this mapping
    if (offload->total_offset_local.find(element_type) ==
        offload->total_offset_local.end()) {
      continue;
    }

    SNode *snode = conv_type == mesh::ConvType::l2g
                       ? offload->mesh->l2g_map.find(element_type)->second
                       : offload->mesh->l2r_map.find(element_type)->second;
    auto data_type = snode->dt.ptr_removed();
    auto dtype_size = data_type_size(data_type);

    // Ensure BLS alignment
    bls_offset_in_bytes +=
        (dtype_size - bls_offset_in_bytes % dtype_size) % dtype_size;

    if (block == nullptr) {
      block = std::make_unique<Block>();
      block->parent_stmt = offload;
    }

    // int i = threadIdx.x;
    // while (x < total_{}_num) {
    //  mapping_shared[i] = mapping[i + total_{}_offset];
    //  x += blockDim.x;
    // }

    // Step 1:
    // Fetch mapping to BLS

    // TODO(changyu): if the target index space is reordered, we can do more
    // optimization
    {
      Stmt *thread_idx_stmt = block->push_back<LoopLinearIndexStmt>(
          offload);  // Equivalent to CUDA threadIdx
      Stmt *idx = block->push_back<AllocaStmt>(data_type);
      [[maybe_unused]] Stmt *init_val =
          block->push_back<LocalStoreStmt>(idx, thread_idx_stmt);
      Stmt *bls_element_offset_bytes = block->push_back<ConstStmt>(
          LaneAttribute<TypedConstant>{(int32)bls_offset_in_bytes});
      Stmt *block_dim_val = block->push_back<ConstStmt>(
          LaneAttribute<TypedConstant>{offload->block_dim});
      Stmt *total_element_num =
          offload->total_num_local.find(element_type)->second;
      Stmt *total_element_offset =
          offload->total_offset_local.find(element_type)->second;

      std::unique_ptr<Block> body = std::make_unique<Block>();
      {
        Stmt *idx_val = body->push_back<LocalLoadStmt>(LocalAddress{idx, 0});
        Stmt *cond = body->push_back<BinaryOpStmt>(BinaryOpType::cmp_lt,
                                                   idx_val, total_element_num);
        { body->push_back<WhileControlStmt>(nullptr, cond); }
        Stmt *idx_val_byte = body->push_back<BinaryOpStmt>(
            BinaryOpType::mul, idx_val,
            body->push_back<ConstStmt>(TypedConstant(dtype_size)));
        Stmt *offset = body->push_back<BinaryOpStmt>(
            BinaryOpType::add, bls_element_offset_bytes, idx_val_byte);
        Stmt *bls_ptr = body->push_back<BlockLocalPtrStmt>(
            offset,
            TypeFactory::create_vector_or_scalar_type(1, data_type, true));
        Stmt *global_offset = body->push_back<BinaryOpStmt>(
            BinaryOpType::add, total_element_offset, idx_val);
        Stmt *global_ptr = body->push_back<GlobalPtrStmt>(
            LaneAttribute<SNode *>{snode}, std::vector<Stmt *>{global_offset});
        Stmt *global_load = body->push_back<GlobalLoadStmt>(global_ptr);
        [[maybe_unused]] Stmt *bls_store =
            body->push_back<GlobalStoreStmt>(bls_ptr, global_load);

        Stmt *idx_val_ = body->push_back<BinaryOpStmt>(BinaryOpType::add,
                                                       idx_val, block_dim_val);
        [[maybe_unused]] Stmt *idx_store =
            body->push_back<LocalStoreStmt>(idx, idx_val_);
      }
      block->push_back<WhileStmt>(std::move(body));
    }

    // Step 2:
    // Make mesh index mapping load from BLS instead of global fields

    // TODO(changyu): before this step, if a mesh attribute field needs to be
    // localized, We should simply remove the `MeshIndexConversionStmt`
    {
      std::vector<MeshIndexConversionStmt *> idx_conv_stmts;

      irpass::analysis::gather_statements(offload->body.get(), [&](Stmt *stmt) {
        if (auto idx_conv = stmt->cast<MeshIndexConversionStmt>()) {
          if (idx_conv->mesh == offload->mesh &&
              idx_conv->conv_type == conv_type) {
            mesh::MeshElementType from_type;
            if (auto idx = idx_conv->idx->cast<LoopIndexStmt>()) {
              from_type = idx->mesh_index_type();
            } else if (auto idx =
                           idx_conv->idx->cast<MeshRelationAccessStmt>()) {
              from_type = idx->to_type;
            } else {
              TI_NOT_IMPLEMENTED;
            }
            if (from_type == element_type) {
              idx_conv_stmts.push_back(idx_conv);
            }
          }
        }
        return false;
      });

      for (auto stmt : idx_conv_stmts) {
        VecStatement bls;
        Stmt *bls_element_offset_bytes = bls.push_back<ConstStmt>(
            LaneAttribute<TypedConstant>{(int32)bls_offset_in_bytes});
        Stmt *idx_byte = bls.push_back<BinaryOpStmt>(
            BinaryOpType::mul, stmt->idx,
            bls.push_back<ConstStmt>(TypedConstant(dtype_size)));
        Stmt *offset = bls.push_back<BinaryOpStmt>(
            BinaryOpType::add, bls_element_offset_bytes, idx_byte);
        Stmt *bls_ptr = bls.push_back<BlockLocalPtrStmt>(
            offset,
            TypeFactory::create_vector_or_scalar_type(1, data_type, true));
        [[maybe_unused]] Stmt *bls_load =
            bls.push_back<GlobalLoadStmt>(bls_ptr);
        stmt->replace_with(std::move(bls));
      }
    }

    // allocate storage for the BLS variable
    bls_offset_in_bytes +=
        dtype_size *
        offload->mesh->patch_max_element_num.find(element_type)->second;
  }

  offload->bls_size = std::max(std::size_t(1), bls_offset_in_bytes);
}

// This pass should happen after offloading but before lower_access
void make_mesh_index_mapping_local(
    IRNode *root,
    const CompileConfig &config,
    const MakeMeshIndexMappingLocal::Args &args) {
  TI_AUTO_PROF;

  // =========================================================================================
  // This pass generates code like this:
  // // Load V_l2g
  // for (int i = threadIdx.x; i < total_vertices; i += blockDim.x) {
  //   V_l2g[i] = _V_l2g[i + total_vertices_offset];
  // }
  // =========================================================================================

  if (auto root_block = root->cast<Block>()) {
    for (auto &offload : root_block->statements) {
      make_mesh_index_mapping_local_offload(offload->cast<OffloadedStmt>(),
                                            config, args.kernel_name);
    }
  } else {
    make_mesh_index_mapping_local_offload(root->as<OffloadedStmt>(), config,
                                          args.kernel_name);
  }

  type_check(root, config);
}

}  // namespace irpass
}  // namespace lang
}  // namespace taichi