#include <fiddle/grid/box_utilities.h>

#include <fiddle/interaction/interaction_base.h>

#include <fiddle/transfer/overlap_partitioning_tools.h>

#include <deal.II/base/array_view.h>
#include <deal.II/base/mpi_noncontiguous_partitioner.templates.h>

#include <deal.II/fe/fe_values.h>

#include <deal.II/numerics/rtree.h>

#include <boost/container/small_vector.hpp>

#include <ibtk/IndexUtilities.h>
#include <ibtk/LEInteractor.h>

#include <memory>
#include <vector>

namespace fdl
{
  using namespace dealii;
  using namespace SAMRAI;

  template <int dim, int spacedim>
  InteractionBase<dim, spacedim>::InteractionBase(
    const parallel::shared::Triangulation<dim, spacedim> &n_tria,
    const std::vector<BoundingBox<spacedim, float>> & global_active_cell_bboxes,
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> p_hierarchy,
    const int                                         l_number,
    std::shared_ptr<IBTK::SAMRAIDataCache>            e_data_cache)
    : communicator(MPI_COMM_NULL)
    , native_tria(&n_tria)
    , patch_hierarchy(p_hierarchy)
    , level_number(l_number)
    , eulerian_data_cache(e_data_cache)
  {
    reinit(
      n_tria, global_active_cell_bboxes, p_hierarchy, l_number, e_data_cache);
  }



  template <int dim, int spacedim>
  void
  InteractionBase<dim, spacedim>::reinit(
    const parallel::shared::Triangulation<dim, spacedim> &n_tria,
    const std::vector<BoundingBox<spacedim, float>> & global_active_cell_bboxes,
    tbox::Pointer<hier::BasePatchHierarchy<spacedim>> p_hierarchy,
    const int                                         l_number,
    std::shared_ptr<IBTK::SAMRAIDataCache>            e_data_cache)
  {
    // We don't need to create a communicator unless its the first time we are
    // here or if we, for some reason, get reinitialized with a totally new
    // Triangulation with a new network
    if (communicator == MPI_COMM_NULL
        || native_tria->get_communicator() != n_tria.get_communicator())
      communicator = Utilities::MPI::duplicate_communicator(
        n_tria.get_communicator());

#ifdef DEBUG
    {
      int result = 0;
      int ierr = MPI_Comm_compare(communicator,
                                  tbox::SAMRAI_MPI::getCommunicator(),
                                  &result);
      AssertThrowMPI(ierr);
      Assert(result == MPI_CONGRUENT || result == MPI_IDENT,
             ExcMessage("The same communicator should be used for the "
                        "triangulation (from deal.II) and in SAMRAI"));
    }
#endif

    native_tria         = &n_tria;
    patch_hierarchy     = p_hierarchy;
    level_number        = l_number;
    eulerian_data_cache = e_data_cache;

    // Check inputs
    Assert(global_active_cell_bboxes.size() == native_tria->n_active_cells(),
           ExcMessage("There should be a bounding box for each active cell"));
    Assert(patch_hierarchy,
           ExcMessage("The provided pointer to a patch hierarchy should not be "
                      "null."));
    AssertIndexRange(l_number, patch_hierarchy->getNumberOfLevels());
    Assert(eulerian_data_cache,
           ExcMessage("The provided shared pointer to an Eulerian data cache "
                      "should not be null."));

    // Set up the patch map:
    {
      const auto patches =
        extract_patches(patch_hierarchy->getPatchLevel(level_number));
      // TODO we need to make extra ghost cell fraction a parameter
      const std::vector<BoundingBox<spacedim>> patch_bboxes =
        compute_patch_bboxes(patches, 1.0);
      BoxIntersectionPredicate<dim, spacedim> predicate(
        global_active_cell_bboxes, patch_bboxes, *native_tria);
      overlap_tria.reinit(*native_tria, predicate);

      std::vector<BoundingBox<spacedim, float>> overlap_bboxes;
      for (const auto &cell : overlap_tria.active_cell_iterators())
        {
          auto native_cell = overlap_tria.get_native_cell(cell);
          overlap_bboxes.push_back(
            global_active_cell_bboxes[native_cell->active_cell_index()]);
        }

      // TODO add the ghost cell width as an input argument to this class
      patch_map.reinit(patches, 1.0, overlap_tria, overlap_bboxes);
    }

    // Set up the active cell index partitioner (for moving cell data):
    {
      IndexSet locally_owned_active_cell_indices(native_tria->n_active_cells());
      IndexSet ghost_active_cell_indices(native_tria->n_active_cells());

      for (const auto cell : native_tria->active_cell_iterators())
        if (cell->is_locally_owned())
          locally_owned_active_cell_indices.add_index(
            cell->active_cell_index());

      // overlap cells are either locally owned or marked as artificial
      for (const auto cell : overlap_tria.active_cell_iterators())
        ghost_active_cell_indices.add_index(
          overlap_tria.get_native_cell(cell)->active_cell_index());

      active_cell_index_partitioner.reinit(locally_owned_active_cell_indices,
                                           ghost_active_cell_indices,
                                           communicator);

      quad_index_work_size =
        active_cell_index_partitioner.temporary_storage_size();

      const auto n_targets  = active_cell_index_partitioner.n_targets();
      n_quad_index_requests = n_targets.first + n_targets.second;
    }
  }



  template <int dim, int spacedim>
  InteractionBase<dim, spacedim>::~InteractionBase()
  {
    int ierr = MPI_Comm_free(&communicator);
    AssertNothrow(ierr == 0, ExcMessage("Unable to free the MPI communicator"));
  }



  template <int dim, int spacedim>
  DoFHandler<dim, spacedim> &
  InteractionBase<dim, spacedim>::get_overlap_dof_handler(
    const DoFHandler<dim, spacedim> &native_dof_handler)
  {
    auto iter = std::find(native_dof_handlers.begin(),
                          native_dof_handlers.end(),
                          &native_dof_handler);
    AssertThrow(iter != native_dof_handlers.end(),
                ExcMessage("The provided dof handler must already be "
                           "registered with this class."));
    return *overlap_dof_handlers[iter - native_dof_handlers.begin()];
  }



  template <int dim, int spacedim>
  const DoFHandler<dim, spacedim> &
  InteractionBase<dim, spacedim>::get_overlap_dof_handler(
    const DoFHandler<dim, spacedim> &native_dof_handler) const
  {
    auto iter = std::find(native_dof_handlers.begin(),
                          native_dof_handlers.end(),
                          &native_dof_handler);
    AssertThrow(iter != native_dof_handlers.end(),
                ExcMessage("The provided dof handler must already be "
                           "registered with this class."));
    return *overlap_dof_handlers[iter - native_dof_handlers.begin()];
  }



  template <int dim, int spacedim>
  Scatter<double> &
  InteractionBase<dim, spacedim>::get_scatter(
    const DoFHandler<dim, spacedim> &native_dof_handler)
  {
    auto iter = std::find(native_dof_handlers.begin(),
                          native_dof_handlers.end(),
                          &native_dof_handler);
    AssertThrow(iter != native_dof_handlers.end(),
                ExcMessage("The provided dof handler must already be "
                           "registered with this class."));
    return scatters[iter - native_dof_handlers.begin()];
  }



  template <int dim, int spacedim>
  void
  InteractionBase<dim, spacedim>::add_dof_handler(
    const DoFHandler<dim, spacedim> &native_dof_handler)
  {
    AssertThrow(&native_dof_handler.get_triangulation() == native_tria,
                ExcMessage("The DoFHandler must use the underlying native "
                           "triangulation."));
    const auto ptr = &native_dof_handler;
    if (std::find(native_dof_handlers.begin(),
                  native_dof_handlers.end(),
                  ptr) == native_dof_handlers.end())
      {
        native_dof_handlers.emplace_back(ptr);
        // TODO - implement a move ctor for DH in deal.II
        overlap_dof_handlers.emplace_back(
          std::make_unique<DoFHandler<dim, spacedim>>(overlap_tria));
        auto &overlap_dof_handler = *overlap_dof_handlers.back();
        overlap_dof_handler.distribute_dofs(
          native_dof_handler.get_fe_collection());

        const std::vector<types::global_dof_index> overlap_to_native_dofs =
          compute_overlap_to_native_dof_translation(overlap_tria,
                                                    overlap_dof_handler,
                                                    native_dof_handler);
        scatters.emplace_back(overlap_to_native_dofs,
                              native_dof_handler.locally_owned_dofs(),
                              communicator);
      }
  }



  template <int dim, int spacedim>
  std::unique_ptr<TransactionBase>
  InteractionBase<dim, spacedim>::compute_projection_rhs_start(
    const int                                         f_data_idx,
    const QuadratureFamily<dim> &                     quad_family,
    const std::vector<unsigned char> &                quad_indices,
    const DoFHandler<dim, spacedim> &                 X_dof_handler,
    const LinearAlgebra::distributed::Vector<double> &X,
    const DoFHandler<dim, spacedim> &                 F_dof_handler,
    const Mapping<dim, spacedim> &                    F_mapping,
    LinearAlgebra::distributed::Vector<double> &      F_rhs)
  {
    Assert(quad_indices.size() == native_tria->n_locally_owned_active_cells(),
           ExcMessage("Each locally owned active cell should have a "
                      "quadrature index"));
#ifdef DEBUG
    {
      int result = 0;
      int ierr = MPI_Comm_compare(communicator, X.get_mpi_communicator(), &result);
      AssertThrowMPI(ierr);
      Assert(result == MPI_CONGRUENT,
             ExcMessage("The same communicator should be used for X and the "
                        "input triangulation"));
      ierr = MPI_Comm_compare(communicator, F_rhs.get_mpi_communicator(), &result);
      AssertThrowMPI(ierr);
      Assert(result == MPI_CONGRUENT,
             ExcMessage("The same communicator should be used for F_rhs and "
                        "the input triangulation"));
    }
#endif

    auto t_ptr = std::make_unique<Transaction<dim, spacedim>>();

    Transaction<dim, spacedim> &transaction = *t_ptr;
    // set up everything we will need later
    transaction.current_f_data_idx = f_data_idx;

    // Setup quadrature info:
    transaction.quad_family         = &quad_family;
    transaction.native_quad_indices = quad_indices;
    transaction.overlap_quad_indices.resize(overlap_tria.n_active_cells());
    transaction.quad_indices_work.resize(quad_index_work_size);
    transaction.quad_indices_requests.resize(n_quad_index_requests);

    // Setup X info:
    transaction.native_X_dof_handler = &X_dof_handler;
    transaction.native_X             = &X;
    transaction.overlap_X_vec.reinit(
      get_overlap_dof_handler(X_dof_handler).n_dofs());

    // Setup F info:
    transaction.native_F_dof_handler = &F_dof_handler;
    transaction.F_mapping            = &F_mapping;
    transaction.native_F_rhs         = &F_rhs;
    transaction.overlap_F_rhs.reinit(
      get_overlap_dof_handler(F_dof_handler).n_dofs());

    // Setup state:
    transaction.next_state = Transaction<dim, spacedim>::State::Intermediate;
    transaction.operation =
      Transaction<dim, spacedim>::Operation::Interpolation;

    // OK, now start scattering:
    Scatter<double> &X_scatter = get_scatter(X_dof_handler);

    // Since we set up our own communicator in this object we can fearlessly use
    // channels 0 and 1 to guarantee traffic is not accidentally mingled
    int channel = 0;
    X_scatter.global_to_overlap_start(*transaction.native_X,
                                      channel,
                                      transaction.overlap_X_vec);
    ++channel;

    active_cell_index_partitioner.export_to_ghosted_array_start<unsigned char>(
      channel,
      make_const_array_view(transaction.native_quad_indices),
      make_array_view(transaction.quad_indices_work),
      transaction.quad_indices_requests);

    return t_ptr;
  }



  template <int dim, int spacedim>
  std::unique_ptr<TransactionBase>
  InteractionBase<dim, spacedim>::compute_projection_rhs_intermediate(
    std::unique_ptr<TransactionBase> t_ptr)
  {
    auto &trans = dynamic_cast<Transaction<dim, spacedim> &>(*t_ptr);
    Assert((trans.operation ==
            Transaction<dim, spacedim>::Operation::Interpolation),
           ExcMessage("Transaction operation should be Interpolation"));
    Assert((trans.next_state ==
            Transaction<dim, spacedim>::State::Intermediate),
           ExcMessage("Transaction state should be Intermediate"));

    Scatter<double> &X_scatter = get_scatter(*trans.native_X_dof_handler);

    X_scatter.global_to_overlap_finish(*trans.native_X, trans.overlap_X_vec);

    active_cell_index_partitioner.export_to_ghosted_array_finish<unsigned char>(
      make_const_array_view(trans.quad_indices_work),
      make_array_view(trans.overlap_quad_indices),
      trans.quad_indices_requests);

    // this is the point at which a base class would normally do computations.

    // After we compute we begin the scatter back to the native partitioning:
    Scatter<double> &F_scatter = get_scatter(*trans.native_F_dof_handler);

    // This object *cannot* get here without the first two scatters finishing so
    // using channel 0 again is fine
    int channel = 0;
    F_scatter.overlap_to_global_start(trans.overlap_F_rhs,
                                      VectorOperation::add,
                                      channel,
                                      *trans.native_F_rhs);

    trans.next_state = Transaction<dim, spacedim>::State::Finish;

    return t_ptr;
  }



  template <int dim, int spacedim>
  void
  InteractionBase<dim, spacedim>::compute_projection_rhs_finish(
    std::unique_ptr<TransactionBase> t_ptr)
  {
    auto &trans = dynamic_cast<Transaction<dim, spacedim> &>(*t_ptr);
    Assert((trans.operation ==
            Transaction<dim, spacedim>::Operation::Interpolation),
           ExcMessage("Transaction operation should be Interpolation"));
    Assert((trans.next_state == Transaction<dim, spacedim>::State::Finish),
           ExcMessage("Transaction state should be Finish"));

    Scatter<double> &F_scatter = get_scatter(*trans.native_F_dof_handler);
    F_scatter.overlap_to_global_finish(trans.overlap_F_rhs,
                                       VectorOperation::add,
                                       *trans.native_F_rhs);
    trans.next_state = Transaction<dim, spacedim>::State::Done;
  }

  // instantiations

  template class InteractionBase<NDIM - 1, NDIM>;
  template class InteractionBase<NDIM, NDIM>;
} // namespace fdl