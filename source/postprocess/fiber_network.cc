#include <fiddle/postprocess/fiber_network.h>
#include <deal.II/distributed/shared_tria.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/grid/tria.h>

namespace fdl
{
    using namespace dealii;

    template <int dim, int spacedim>
    std::vector<dealii::Tensor<1, spacedim>> &
    FiberNetwork<dim, spacedim>::get_fibers(typename Triangulation<dim, spacedim>::active_cell_iterator &cell) const
    {           
        // calculate the correct vector entry
        auto cell_index = cell->global_active_cell_index() - local_processor_min_cell_index; 
        
        std::vector<dealii::Tensor<1, spacedim>> * fiber_ptr = new std::vector<dealii::Tensor<1, spacedim>>;

        for(auto j=0; j<(this->fibers).size(1); j++){
           fiber_ptr->push_back((this->fibers)(cell_index,j));
        }
         
        return *fiber_ptr;
    }


}
