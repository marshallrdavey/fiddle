#include <fiddle/grid/box_utilities.h>

#include <fiddle/interaction/ifed_method.h>
#include <fiddle/interaction/interaction_utilities.h>

#include <deal.II/distributed/shared_tria.h>

#include <deal.II/fe/mapping_fe_field.h>

namespace fdl
{
  using namespace SAMRAI;
  using namespace dealii;

  template <int dim, int spacedim>
  void
  IFEDMethod<dim, spacedim>::applyGradientDetector(
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> hierarchy,
    int                                               level_number,
    double /*error_data_time*/,
    int tag_index,
    bool /*initial_time*/,
    bool /*uses_richardson_extrapolation_too*/)
  {
    // TODO: we should find a way to save the bboxes so they do not need to be
    // computed for each level that needs tagging - conceivably this could
    // happen in beginDataRedistribution() and the array can be cleared in
    // endDataRedistribution()
    for (const Part<dim, spacedim> &part : parts)
      {
        const DoFHandler<dim, spacedim> &dof_handler = part.get_dof_handler();
        MappingFEField<dim,
                       spacedim,
                       LinearAlgebra::distributed::Vector<double>>
                   mapping(dof_handler, part.get_position());
        const auto local_bboxes = compute_cell_bboxes(dof_handler, mapping);
        // Like most other things this only works with p::S::T now
        const auto &tria =
          dynamic_cast<const parallel::shared::Triangulation<dim, spacedim> &>(
            part.get_triangulation());
        const auto global_bboxes =
          collect_all_active_cell_bboxes(tria, local_bboxes);
        tbox::Pointer<hier::PatchLevel<spacedim>> patch_level =
          hierarchy->getPatchLevel(level_number);
        Assert(patch_level, ExcNotImplemented());
        tag_cells(global_bboxes, tag_index, patch_level);
      }
  }

  template class IFEDMethod<NDIM - 1, NDIM>;
  template class IFEDMethod<NDIM, NDIM>;
} // namespace fdl