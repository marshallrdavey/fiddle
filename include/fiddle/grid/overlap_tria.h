#ifndef included_fiddle_overlap_tria_h
#define included_fiddle_overlap_tria_h

#include <deal.II/base/bounding_box.h>
#include <deal.II/base/subscriptor.h>

#include <deal.II/distributed/shared_tria.h>

#include <deal.II/grid/tria.h>

#include <vector>

namespace fdl
{
  using namespace dealii;

  //
  // Class describing a Triangulation built from a shared Triangulation.
  //
  template <int dim, int spacedim = dim>
  class OverlapTriangulation : public dealii::Triangulation<dim, spacedim>
  {
    using active_cell_iterator =
      typename dealii::Triangulation<dim, spacedim>::active_cell_iterator;
    using cell_iterator =
      typename dealii::Triangulation<dim, spacedim>::cell_iterator;

  public:
    OverlapTriangulation(
      const parallel::shared::Triangulation<dim, spacedim> &shared_tria,
      const std::vector<BoundingBox<spacedim>> &            patch_bboxes);

    virtual types::subdomain_id
    locally_owned_subdomain() const;

    void
    reinit(const parallel::shared::Triangulation<dim, spacedim> &shared_tria,
           const std::vector<BoundingBox<spacedim>> &            patch_bboxes);

    const parallel::shared::Triangulation<dim, spacedim> &
    get_native_triangulation() const;

    /**
     * Get the native cell iterator equivalent to the current cell iterator.
     */
    inline cell_iterator
    get_native_cell(const cell_iterator &cell) const;

    /**
     * Get the active cell iterators in order of ascending corresponding native
     * active cell index.
     *
     * @todo replace this with something that stores cell {level, index} pairs
     * instead to lower memory usage (we don't need to store multiple pointers
     * to the triangulation).
     */
    const std::vector<active_cell_iterator> &
    get_cell_iterators_in_active_native_order() const;

  protected:
    /**
     * Utility function that stores a native cell and returns its array index
     * (which will then be set as the user index or material id).
     */
    std::size_t
    add_native_cell(const cell_iterator &cell);

    void
    reinit_overlapping_tria(
      const std::vector<BoundingBox<spacedim>> &patch_bboxes);

    /**
     * Pointer to the Triangulation which describes the whole domain.
     */
    SmartPointer<const parallel::shared::Triangulation<dim, spacedim>,
                 OverlapTriangulation<dim, spacedim>>
      native_tria;

    /**
     * Level and index pairs (i.e., enough to create an iterator) of native
     * cells which have an equivalent cell on this triangulation.
     */
    std::vector<std::pair<int, int>> native_cells;

    /**
     * Active cell iterators sorted by the active cell index of the
     * corresponding native cell. Useful for doing data transfer.
     */
    std::vector<active_cell_iterator> cell_iterators_in_active_native_order;
  };


  //
  // inline functions
  //
  template <int dim, int spacedim>
  const parallel::shared::Triangulation<dim, spacedim> &
  OverlapTriangulation<dim, spacedim>::get_native_triangulation() const
  {
    return *native_tria;
  }



  template <int dim, int spacedim>
  inline typename OverlapTriangulation<dim, spacedim>::cell_iterator
  OverlapTriangulation<dim, spacedim>::get_native_cell(
    const cell_iterator &cell) const
  {
    AssertIndexRange(cell->user_index(), native_cells.size());
    const auto          pair = native_cells[cell->user_index()];
    const cell_iterator native_cell(native_tria, pair.first, pair.second);
    Assert((native_cell->barycenter() - cell->barycenter()).norm() < 1e-12,
           ExcInternalError());
    return native_cell;
  }



  template <int dim, int spacedim>
  inline const std::vector<
    typename OverlapTriangulation<dim, spacedim>::active_cell_iterator> &
  OverlapTriangulation<dim,
                       spacedim>::get_cell_iterators_in_active_native_order()
    const
  {
    return cell_iterators_in_active_native_order;
  }



  template <int dim, int spacedim>
  inline std::size_t
  OverlapTriangulation<dim, spacedim>::add_native_cell(
    const cell_iterator &cell)
  {
    Assert(&cell->get_triangulation() == native_tria,
           ExcMessage("should be a native cell"));
    native_cells.emplace_back(cell->level(), cell->index());
    return native_cells.size() - 1;
  }
} // namespace fdl

#endif // define included_fiddle_overlap_tria_h