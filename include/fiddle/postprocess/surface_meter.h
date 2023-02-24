#ifndef included_fiddle_postprocess_meter_mesh_h
#define included_fiddle_postprocess_meter_mesh_h

#include <fiddle/base/config.h>

#include <fiddle/interaction/nodal_interaction.h>

#include <fiddle/postprocess/point_values.h>

#include <deal.II/base/point.h>
#include <deal.II/base/smartpointer.h>
#include <deal.II/base/tensor.h>

#include <deal.II/dofs/dof_handler.h>

#include <deal.II/fe/mapping.h>

#include <deal.II/lac/la_parallel_vector.h>

#include <memory>
#include <vector>

namespace fdl
{
  using namespace dealii;

  /**
   * Class for integrating Cartesian-grid values on codimension one surfaces
   * (colloquially a 'meter mesh').
   *
   * This class constructs a codimension one mesh in a dimension-dependent way:
   *
   * - in 3D, the provided points are treated as a closed loop surrounding some
   *   surface. Nearest points will be connected by line segments to form the
   *   boundary of a triangulation.
   * - in 2D, the provided points are treated as line segments - i.e., each
   *   adjacent pair of points define at least one element.
   *
   * This is because, in 2D, one may want to create a meter mesh corresponding
   * to a line rather than a closed loop.  To make a closed loop in 2D simply
   * make the first and last points equal.
   *
   * In both cases, the Triangulation created by this class will have elements
   * with side lengths approximately equal to the Cartesian grid cell length
   * (i.e., MFAC = 1).
   *
   * The velocity of the meter is the mean velocity of the boundary of the
   * meter - e.g., for channel flow, one can specify a mesh with points on the
   * top and bottom of the channel and then the meter velocity will equal the
   * wall velocity. This choice lets one compute fluxes through the meter
   * correctly (as the reference frame has a nonzero velocity). To get
   * absolute instead of relative fluxes simply set the input velocity values
   * to zero.
   *
   * @warning Due to the way IBAMR computes cell indices, points lying on the
   * upper boundaries of the computational domain may not have correct
   * interpolated values. If you want to compute values on the upper boundary
   * then you should adjust your points slightly using, e.g.,
   * std::nexttoward().
   */
  template <int dim, int spacedim = dim>
  class SurfaceMeter
  {
  public:
    /**
     * Constructor.
     *
     * @param[in] mapping Mapping defined in reference coordinates (e.g., the
     * mapping returned by Part::get_mapping())
     *
     * @param[in] position_dof_handler DoFHandler describing the position and
     * velocity finite element spaces.
     *
     * @param[in] convex_hull Points, in reference coordinates, describing the
     * boundary of the meter mesh. These points typically outline a disk and
     * typically come from a node set defined on the Triangulation associated
     * with @p dof_handler.
     *
     * @warning This function uses PointValues to compute the positions of the
     * nodes, which may, in parallel, give slightly different results (on the
     * level of machine precision) based on the cell partitioning. In unusual
     * cases this can cause Triangle to generate slightly different
     * triangulations - i.e., the exact meter Triangulation may depend on the
     * number of processors.
     */
    SurfaceMeter(
      const Mapping<dim, spacedim>                     &mapping,
      const DoFHandler<dim, spacedim>                  &position_dof_handler,
      const std::vector<Point<spacedim>>               &convex_hull,
      tbox::Pointer<hier::BasePatchHierarchy<spacedim>> patch_hierarchy,
      const LinearAlgebra::distributed::Vector<double> &position,
      const LinearAlgebra::distributed::Vector<double> &velocity);

    /**
     * Alternate constructor which uses purely nodal data instead of finite
     * element fields.
     */
    SurfaceMeter(
      const std::vector<Point<spacedim>>               &convex_hull,
      const std::vector<Tensor<1, spacedim>>           &velocity,
      tbox::Pointer<hier::BasePatchHierarchy<spacedim>> patch_hierarchy);

    /**
     * Reinitialize the meter mesh to have its coordinates specified by @p
     * position and velocity by @p velocity.
     *
     * @note This function may only be called if the object was originally set
     * up with finite element data.
     */
    void
    reinit(const LinearAlgebra::distributed::Vector<double> &position,
           const LinearAlgebra::distributed::Vector<double> &velocity);

    /**
     * Alternative reinitialization function which (like the alternative
     * constructor) uses purely nodal data.
     */
    void
    reinit(const std::vector<Point<spacedim>>     &convex_hull,
           const std::vector<Tensor<1, spacedim>> &velocity);

    /**
     * Return a reference to the meter Triangulation. This triangulation is
     * not in reference coordinates: instead its absolute position is
     * determined by the position vector specified to the constructor or
     * reinit().
     */
    const Triangulation<dim - 1, spacedim> &
    get_triangulation() const;

    /**
     * Return a reference to the Mapping used on the meter mesh.
     */
    const Mapping<dim - 1, spacedim> &
    get_mapping() const;

    /**
     * Return a reference to the DoFHandler for scalar fields.
     */
    const DoFHandler<dim - 1, spacedim> &
    get_scalar_dof_handler() const;

    /**
     * Return a reference to the DoFHandler for vector fields.
     */
    const DoFHandler<dim - 1, spacedim> &
    get_vector_dof_handler() const;

    /**
     * Interpolate a scalar-valued quantity.
     */
    LinearAlgebra::distributed::Vector<double>
    interpolate_scalar_field(const int          data_idx,
                             const std::string &kernel_name) const;

    /**
     * Interpolate a vector-valued quantity.
     */
    LinearAlgebra::distributed::Vector<double>
    interpolate_vector_field(const int          data_idx,
                             const std::string &kernel_name) const;

    /**
     * Return the mean velocity of the meter itself computed from the inputs
     * to the ctor or reinit() functions.
     *
     *
     * This value is computed in one of two ways:
     * - If the object is initialized from pointwise data, then the mean
     *   velocity is simply the average of the provided velocities.
     *
     * - If the object is initialized from FE field data, then in 2D this is
     *   the average of the pointwise velocities. In 3D it is the mean value
     *   of the velocity field computed on the boundary.
     */
    Tensor<1, spacedim>
    get_mean_velocity() const;

    /**
     * Compute the mean value of some scalar-valued quantity.
     *
     * @param[in] data_idx Some data index corresponding to data on the
     * Cartesian grid. This class will copy the data into a scratch index and
     * update ghost data.
     */
    double
    compute_mean_value(const int data_idx, const std::string &kernel_name);

    /**
     * Return the centroid of the meter mesh. This point may not be inside the
     * mesh.
     */
    Point<spacedim>
    get_centroid() const;

  protected:
    void
    reinit_tria(const std::vector<Point<spacedim>> &convex_hull);

    void
    reinit_mean_velocity(
      const std::vector<Tensor<1, spacedim>> &velocity_values);

    /**
     * Original Mapping.
     */
    SmartPointer<const Mapping<dim, spacedim>> mapping;

    /**
     * Original DoFHandler.
     */
    SmartPointer<const DoFHandler<dim, spacedim>> position_dof_handler;

    /**
     * Mapping on the meter Triangulation.
     */
    std::unique_ptr<Mapping<dim - 1, spacedim>> meter_mapping;

    /**
     * Quadrature to use on the meter mesh. Has degree $2 * scalar_fe.degree +
     * 1$.
     */
    Quadrature<dim - 1> meter_quadrature;

    /**
     * Cartesian-grid data.
     */
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> patch_hierarchy;

    /**
     * PointValues object for computing the mesh's position.
     */
    std::unique_ptr<PointValues<spacedim, dim, spacedim>> point_values;

    /**
     * Meter Triangulation.
     */
    parallel::shared::Triangulation<dim - 1, spacedim> meter_tria;

    /**
     * Positions of the mesh DoFs - always the identity function after
     * reinitalization.
     */
    LinearAlgebra::distributed::Vector<double> identity_position;

    /**
     * Mean meter velocity.
     */
    Tensor<1, spacedim> mean_velocity;

    /**
     * Meter centroid.
     */
    Point<spacedim> centroid;

    /**
     * Scalar FiniteElement used on meter_tria
     */
    std::unique_ptr<FiniteElement<dim - 1, spacedim>> scalar_fe;

    /**
     * Vector FiniteElement used on meter_tria
     */
    std::unique_ptr<FiniteElement<dim - 1, spacedim>> vector_fe;

    /**
     * DoFHandler for scalar quantities defined on meter_tria.
     */
    DoFHandler<dim - 1, spacedim> scalar_dof_handler;

    /**
     * DoFHandler for vector-valued quantities defined on meter_tria.
     */
    DoFHandler<dim - 1, spacedim> vector_dof_handler;

    std::shared_ptr<Utilities::MPI::Partitioner> vector_partitioner;

    std::shared_ptr<Utilities::MPI::Partitioner> scalar_partitioner;

    /**
     * Interaction object.
     */
    std::unique_ptr<NodalInteraction<dim - 1, spacedim>> nodal_interaction;
  };

  template <int dim, int spacedim>
  Point<spacedim>
  SurfaceMeter<dim, spacedim>::get_centroid() const
  {
    return centroid;
  }

  template <int dim, int spacedim>
  Tensor<1, spacedim>
  SurfaceMeter<dim, spacedim>::get_mean_velocity() const
  {
    return mean_velocity;
  }

  template <int dim, int spacedim>
  inline const Mapping<dim - 1, spacedim> &
  SurfaceMeter<dim, spacedim>::get_mapping() const
  {
    return *meter_mapping;
  }

  template <int dim, int spacedim>
  inline const Triangulation<dim - 1, spacedim> &
  SurfaceMeter<dim, spacedim>::get_triangulation() const
  {
    return meter_tria;
  }

  template <int dim, int spacedim>
  inline const DoFHandler<dim - 1, spacedim> &
  SurfaceMeter<dim, spacedim>::get_scalar_dof_handler() const
  {
    return scalar_dof_handler;
  }

  template <int dim, int spacedim>
  inline const DoFHandler<dim - 1, spacedim> &
  SurfaceMeter<dim, spacedim>::get_vector_dof_handler() const
  {
    return vector_dof_handler;
  }
} // namespace fdl

#endif