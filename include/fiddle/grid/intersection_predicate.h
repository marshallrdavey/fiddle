#ifndef included_fiddle_intersection_predicate_h
#define included_fiddle_intersection_predicate_h

#include <deal.II/base/bounding_box.h>

#include <deal.II/dofs/dof_handler.h>

#include <deal.II/grid/tria.h>

#include <fiddle/base/exceptions.h>

#include <mpi.h>

namespace fdl
{
  using namespace dealii;

  // todo - add it to deal.II
  template <int spacedim, typename Number1, typename Number2>
  bool
  intersects(const BoundingBox<spacedim, Number1> &a,
             const BoundingBox<spacedim, Number2> &b)
  {
    // Since boxes are tensor products of line intervals it suffices to check
    // that the line segments for each coordinate axis overlap.
    for (unsigned int d = 0; d < spacedim; ++d)
      {
        // Line segments can intersect in two ways:
        // 1. They can overlap.
        // 2. One can be inside the other.
        //
        // In the first case we want to see if either end point of the second
        // line segment lies within the first. In the second case we can simply
        // check that one end point of the first line segment lies in the second
        // line segment. Note that we don't need, in the second case, to do two
        // checks since that case is already covered by the first.
        if (!((a.lower_bound(d) <= b.lower_bound(d) &&
               b.lower_bound(d) <= a.upper_bound(d)) ||
              (a.lower_bound(d) <= b.upper_bound(d) &&
               b.upper_bound(d) <= a.upper_bound(d))) &&
            !((b.lower_bound(d) <= a.lower_bound(d) &&
               a.lower_bound(d) <= b.upper_bound(d))))
          {
            return false;
          }
      }

    return true;
  }

  /**
   * Class which can determine whether or not a given cell intersects some
   * geometric object.
   */
  template <int dim, int spacedim = dim>
  class IntersectionPredicate
  {
  public:
    virtual bool
    operator()(const typename Triangulation<dim, spacedim>::cell_iterator &cell)
      const = 0;

    virtual ~IntersectionPredicate() = default;
  };

  /**
   * Intersection predicate that determines intersections based on the locations
   * of cells in the Triangulation and nothing else.
   */
  template <int dim, int spacedim = dim>
  class TriaIntersectionPredicate : public IntersectionPredicate<dim, spacedim>
  {
  public:
    TriaIntersectionPredicate(const std::vector<BoundingBox<spacedim>> &bboxes)
      : bounding_boxes(bboxes)
    {
      // TODO: build an rtree here.
    }

    virtual bool
    operator()(const typename Triangulation<dim, spacedim>::cell_iterator &cell)
      const override
    {
      const auto cell_bbox = cell->bounding_box();
      for (const auto &bbox : bounding_boxes)
        if (intersects(cell_bbox, bbox))
          return true;
      return false;
    }

  protected:
    std::vector<BoundingBox<spacedim>> bounding_boxes;
  };

  /**
   * Intersection predicate based on a displacement from a finite element field.
   */
  template <int dim, int spacedim = dim>
  class FEIntersectionPredicate : public IntersectionPredicate<dim, spacedim>
  {
  public:
    FEIntersectionPredicate(const std::vector<BoundingBox<spacedim>> &bboxes,
                            const MPI_Comm &                 communicator,
                            const DoFHandler<dim, spacedim> &dof_handler,
                            const Mapping<dim, spacedim> &   mapping)
      : tria(&dof_handler.get_triangulation())
      , patch_bboxes(bboxes)
    {
      // TODO: support multiple FEs
      const FiniteElement<dim> &fe = dof_handler.get_fe();
      // TODO: also check bboxes by position of quadrature points instead of
      // just nodes. Use QProjector to place points solely on cell boundaries.
      const Quadrature<dim> nodal_quad(fe.get_unit_support_points());

      FEValues<dim, spacedim> fe_values(mapping,
                                        fe,
                                        nodal_quad,
                                        update_quadrature_points);

      active_cell_bboxes.resize(
        dof_handler.get_triangulation().n_active_cells());
      for (const auto cell : dof_handler.active_cell_iterators())
        if (cell->is_locally_owned())
          {
            fe_values.reinit(cell);
            const BoundingBox<spacedim> dbox(fe_values.get_quadrature_points());
            BoundingBox<spacedim, float> fbox;
            fbox.get_boundary_points() = dbox.get_boundary_points();
            active_cell_bboxes[cell->active_cell_index()] = fbox;
          }

      // TODO: use rtrees in parallel so that we don't need every bbox on every
      // processor in this intermediate step
      constexpr auto n_floats_per_bbox = spacedim * 2;
      static_assert(sizeof(active_cell_bboxes[0]) ==
                      sizeof(float) * n_floats_per_bbox,
                    "packing failed");
      const auto size = n_floats_per_bbox * active_cell_bboxes.size();
      // TODO assert sizes are all equal and nonzero
      const int ierr =
        MPI_Allreduce(MPI_IN_PLACE,
                      reinterpret_cast<float *>(&active_cell_bboxes[0]),
                      size,
                      MPI_FLOAT,
                      MPI_SUM,
                      communicator);
      AssertThrowMPI(ierr);

      for (const auto &bbox : active_cell_bboxes)
        {
          Assert(bbox.volume() > 0, ExcMessage("bboxes should not be empty"));
        }
    }

    virtual bool
    operator()(const typename Triangulation<dim, spacedim>::cell_iterator &cell)
      const override
    {
      Assert(&cell->get_triangulation() == tria,
             ExcMessage("only valid for inputs constructed from the originally "
                        "provided Triangulation"));
      // If the cell is active check its bbox:
      if (cell->is_active())
        {
          const auto &cell_bbox = active_cell_bboxes[cell->active_cell_index()];
          for (const auto &bbox : patch_bboxes)
            if (intersects(cell_bbox, bbox))
              return true;
          return false;
        }
      // Otherwise see if it has a descendant that intersects:
      else if (cell->has_children())
        {
          const auto n_children             = cell->n_children();
          bool       has_intersecting_child = false;
          for (unsigned int child_n = 0; child_n < n_children; ++child_n)
            {
              const bool child_intersects = (*this)(cell->child(child_n));
              if (child_intersects)
                {
                  has_intersecting_child = true;
                  break;
                }
            }
          return has_intersecting_child;
        }
      else
        {
          Assert(false, ExcNotImplemented());
        }

      Assert(false, ExcFDLInternalError());
      return false;
    }

    const SmartPointer<const Triangulation<dim, spacedim>> tria;
    const std::vector<BoundingBox<spacedim>>               patch_bboxes;
    std::vector<BoundingBox<spacedim, float>>              active_cell_bboxes;
  };
} // namespace fdl

#endif