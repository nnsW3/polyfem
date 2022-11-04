#include "ShapeProblem.hpp"

#include <polyfem/utils/Types.hpp>
#include <polyfem/utils/Timer.hpp>
#include <polyfem/utils/MatrixUtils.hpp>

#include <igl/writeOBJ.h>
#include <igl/writeMESH.h>
#include <igl/write_triangle_mesh.h>
#include <igl/avg_edge_length.h>

#include <ipc/ipc.hpp>
#include <ipc/barrier/barrier.hpp>
#include <ipc/barrier/adaptive_stiffness.hpp>
#include <ipc/utils/world_bbox_diagonal_length.hpp>
#include <polyfem/utils/BoundarySampler.hpp>

#include <filesystem>

namespace ipc
{
	NLOHMANN_JSON_SERIALIZE_ENUM(
		ipc::BroadPhaseMethod,
		{{ipc::BroadPhaseMethod::HASH_GRID, "hash_grid"}, // also default
		 {ipc::BroadPhaseMethod::HASH_GRID, "HG"},
		 {ipc::BroadPhaseMethod::BRUTE_FORCE, "brute_force"},
		 {ipc::BroadPhaseMethod::BRUTE_FORCE, "BF"},
		 {ipc::BroadPhaseMethod::SPATIAL_HASH, "spatial_hash"},
		 {ipc::BroadPhaseMethod::SPATIAL_HASH, "SH"},
		 {ipc::BroadPhaseMethod::SWEEP_AND_TINIEST_QUEUE, "sweep_and_tiniest_queue"},
		 {ipc::BroadPhaseMethod::SWEEP_AND_TINIEST_QUEUE, "STQ"},
		 {ipc::BroadPhaseMethod::SWEEP_AND_TINIEST_QUEUE_GPU, "sweep_and_tiniest_queue_gpu"},
		 {ipc::BroadPhaseMethod::SWEEP_AND_TINIEST_QUEUE_GPU, "STQ_GPU"}})
} // namespace ipc

namespace polyfem
{
	namespace
	{
		double triangle_jacobian(const Eigen::VectorXd &v1, const Eigen::VectorXd &v2, const Eigen::VectorXd &v3)
		{
			Eigen::VectorXd a = v2 - v1, b = v3 - v1;
			return a(0) * b(1) - b(0) * a(1);
		}

		double tet_determinant(const Eigen::VectorXd &v1, const Eigen::VectorXd &v2, const Eigen::VectorXd &v3, const Eigen::VectorXd &v4)
		{
			Eigen::Matrix3d mat;
			mat.col(0) << v2 - v1;
			mat.col(1) << v3 - v1;
			mat.col(2) << v4 - v1;
			return mat.determinant();
		}

		bool is_flipped(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F)
		{
			if (F.cols() == 3)
			{
				for (int i = 0; i < F.rows(); i++)
					if (triangle_jacobian(V.row(F(i, 0)), V.row(F(i, 1)), V.row(F(i, 2))) <= 0)
						return true;
			}
			else if (F.cols() == 4)
			{
				for (int i = 0; i < F.rows(); i++)
					if (tet_determinant(V.row(F(i, 0)), V.row(F(i, 1)), V.row(F(i, 2)), V.row(F(i, 3))) <= 0)
						return true;
			}
			else
			{
				return true;
			}

			return false;
		}

		void scaled_jacobian(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, Eigen::VectorXd &quality)
		{
			const int dim = F.cols() - 1;

			quality.setZero(F.rows());
			if (dim == 2)
			{
				for (int i = 0; i < F.rows(); i++)
				{
					Eigen::RowVector3d e0 = V.row(F(i, 2)) - V.row(F(i, 1));
					Eigen::RowVector3d e1 = V.row(F(i, 0)) - V.row(F(i, 2));
					Eigen::RowVector3d e2 = V.row(F(i, 1)) - V.row(F(i, 0));

					double l0 = e0.norm();
					double l1 = e1.norm();
					double l2 = e2.norm();

					double A = 0.5 * (e0.cross(e1)).norm();
					double Lmax = std::max(l0 * l1, std::max(l1 * l2, l0 * l2));

					quality(i) = 2 * A * (2 / sqrt(3)) / Lmax;
				}
			}
			else
			{
				for (int i = 0; i < F.rows(); i++)
				{
					Eigen::RowVector3d e0 = V.row(F(i, 1)) - V.row(F(i, 0));
					Eigen::RowVector3d e1 = V.row(F(i, 2)) - V.row(F(i, 1));
					Eigen::RowVector3d e2 = V.row(F(i, 0)) - V.row(F(i, 2));
					Eigen::RowVector3d e3 = V.row(F(i, 3)) - V.row(F(i, 0));
					Eigen::RowVector3d e4 = V.row(F(i, 3)) - V.row(F(i, 1));
					Eigen::RowVector3d e5 = V.row(F(i, 3)) - V.row(F(i, 2));

					double l0 = e0.norm();
					double l1 = e1.norm();
					double l2 = e2.norm();
					double l3 = e3.norm();
					double l4 = e4.norm();
					double l5 = e5.norm();

					double J = std::abs((e0.cross(e3)).dot(e2));

					double a1 = l0 * l2 * l3;
					double a2 = l0 * l1 * l4;
					double a3 = l1 * l2 * l5;
					double a4 = l3 * l4 * l5;

					double a = std::max({a1, a2, a3, a4, J});
					quality(i) = J * sqrt(2) / a;
				}
			}
		}

		bool internal_smoothing(const Eigen::MatrixXd &V, const Eigen::MatrixXi &F, const std::vector<int> &boundary_indices, const Eigen::MatrixXd &boundary_constraints, const json &slim_params, Eigen::MatrixXd &smooth_field)
		{
			const int dim = F.cols() - 1;
			igl::SLIMData slim_data;
			double soft_const_p = slim_params["soft_p"];
			slim_data.exp_factor = slim_params["exp_factor"];
			Eigen::MatrixXd V_extended;
			V_extended.setZero(V.rows(), 3);
			V_extended.block(0, 0, V.rows(), dim) = V;
			Eigen::VectorXi boundary_indices_ = Eigen::VectorXi::Map(boundary_indices.data(), boundary_indices.size());
			igl::slim_precompute(
				V_extended,
				F,
				V,
				slim_data,
				igl::SYMMETRIC_DIRICHLET,
				boundary_indices_,
				boundary_constraints,
				soft_const_p);

			smooth_field.setZero(V.rows(), V.cols());

			auto is_good_enough = [](const Eigen::MatrixXd &V, const Eigen::VectorXi &b, const Eigen::MatrixXd &C, double &error, double tol = 1e-5) {
				error = 0.0;

				for (unsigned i = 0; i < b.rows(); i++)
					error += (C.row(i) - V.row(b(i))).squaredNorm();

				return error < tol;
			};

			double error = 0;
			int max_it = dim == 2 ? 20 : 50;
			int it = 0;
			bool good_enough = false;

			do
			{
				igl::slim_solve(slim_data, slim_params["min_iter"]);
				good_enough = is_good_enough(slim_data.V_o, boundary_indices_, boundary_constraints, error, slim_params["tol"]);
				smooth_field = slim_data.V_o.block(0, 0, smooth_field.rows(), dim);
				it += slim_params["min_iter"].get<int>();
			} while (it < max_it && !good_enough);

			for (unsigned i = 0; i < boundary_indices_.rows(); i++)
				smooth_field.row(boundary_indices_(i)) = boundary_constraints.row(i);

			logger().debug("SLIM finished in {} iterations", it);

			if (!good_enough)
				logger().warn("Slimflator could not inflate correctly.");

			return good_enough;
		}
	} // namespace

	ShapeProblem::ShapeProblem(State &state_, const std::shared_ptr<CompositeFunctional> j_) : OptimizationProblem(state_, j_)
	{
		optimization_name = "shape";
		const auto &gbases = state.geom_bases();

		// volume constraint
		has_volume_constraint = false;
		for (const auto &param : opt_params["functionals"])
		{
			if (param["type"] == "stress" || param["type"] == "trajectory")
			{
				target_weight = param.value("weight", 1.0);
			}
			if (param["type"] == "volume_constraint")
			{
				volume_params = param;
				has_volume_constraint = true;
				j_volume = CompositeFunctional::create("Volume");
				auto &func_volume = *dynamic_cast<VolumeFunctional *>(j_volume.get());
				func_volume.set_max_volume(volume_params["soft_bound"][1]);
				func_volume.set_min_volume(volume_params["soft_bound"][0]);
			}
		}

		// mesh topology
		state.get_vf(V_rest, elements);

		// contact
		const auto &opt_contact_params = state.args["optimization"]["solver"]["contact"];
		has_collision = opt_contact_params["enabled"];
		if (state.is_contact_enabled() && !has_collision)
			logger().warn("Problem has collision, but collision detection in shape optimization is disabled!");
		if (has_collision)
		{
			_dhat = opt_contact_params["dhat"];
			_prev_distance = -1;
			_barrier_stiffness = opt_contact_params["barrier_stiffness"];
			_broad_phase_method = opt_contact_params["CCD"]["broad_phase"];
			_ccd_tolerance = opt_contact_params["CCD"]["tolerance"];
			_ccd_max_iterations = opt_contact_params["CCD"]["max_iterations"];
		}

		Eigen::MatrixXd boundary_nodes_pos;
		state.build_collision_mesh(boundary_nodes_pos, collision_mesh, state.n_geom_bases, gbases);

		// boundary smoothing
		has_boundary_smoothing = false;
		for (const auto &param : opt_params["functionals"])
		{
			if (param["type"] == "boundary_smoothing")
			{
				boundary_smoothing_params = param;

				if (param["scale_invariant"].get<bool>())
					boundary_smoother.p = boundary_smoothing_params.value("power", 2);
				boundary_smoother.dim = dim;
				boundary_smoother.build_laplacian(state.n_geom_bases, state.mesh->dimension(), collision_mesh.edges(), state.boundary_gnodes, fixed_nodes);
				has_boundary_smoothing = true;
				break;
			}
		}
		if (!has_boundary_smoothing)
			logger().warn("Shape optimization without boundary smoothing!");

		// SLIM
		for (const auto &param : opt_params["parameters"])
		{
			if (param["type"] == "shape")
			{
				shape_params = param;
				if (shape_params.contains("smoothing_paramters"))
					slim_params = shape_params["smoothing_paramters"];
				break;
			}
		}
		if (slim_params.empty())
		{
			slim_params = json::parse(R"(
			{
				"min_iter" : 2,
				"tol" : 1e-8,
				"soft_p" : 1e5,
				"exp_factor" : 5
			}
			)");
		}

		build_fixed_nodes();
		build_tied_nodes();

		if (shape_params["dimensions"].is_array())
			free_dimension = shape_params["dimensions"].get<std::vector<bool>>();
		if (free_dimension.size() < this->dim)
			free_dimension.assign(this->dim, true);

		// constraints on optimization
		x_to_param = [this](const TVector &x, const Eigen::MatrixXd &V_prev, Eigen::MatrixXd &V) {
			V.setZero(x.size() / this->dim, this->dim);
			for (int i = 0; i < V.rows(); i++)
				for (int d = 0; d < this->dim; d++)
					V(i, d) = x(i * this->dim + d);

			for (const auto &pair : this->tied_nodes)
				for (int d = 0; d < this->dim; d++)
					V(pair[1], d) = V(pair[0], d);
		};
		param_to_x = [this](TVector &x, const Eigen::MatrixXd &V) {
			x.setZero(V.rows() * this->dim, 1);
			for (int i = 0; i < V.rows(); i++)
				for (int d = 0; d < this->dim; d++)
					x(i * this->dim + d) = V(i, d);
		};
		dparam_to_dx = [this](TVector &grad_x, const TVector &grad_v) {
			grad_x = grad_v;
			for (int b : this->fixed_nodes)
				for (int d = 0; d < this->dim; d++)
					grad_x(b * this->dim + d) = 0;

			const int size = grad_v.size() / this->dim;
			for (int d = 0; d < this->dim; d++)
				if (!free_dimension[d])
					for (int i = 0; i < size; i++)
						grad_x(i * this->dim + d) = 0;

			for (const auto &pair : this->tied_nodes)
			{
				grad_x(Eigen::seqN(pair[0] * this->dim, this->dim)) += grad_x(Eigen::seqN(pair[1] * this->dim, this->dim));
				grad_x(Eigen::seqN(pair[1] * this->dim, this->dim)).setZero();
			}
		};
	}

	void ShapeProblem::update_constraint_set(const Eigen::MatrixXd &displaced_surface)
	{
		// Store the previous value used to compute the constraint set to avoid
		// duplicate computation.
		static Eigen::MatrixXd cached_displaced_surface;
		if (cached_displaced_surface.size() == displaced_surface.size()
			&& cached_displaced_surface == displaced_surface)
			return;

		if (_use_cached_candidates)
			_constraint_set.build(
				_candidates, collision_mesh, displaced_surface, _dhat);
		else
			_constraint_set.build(
				collision_mesh, displaced_surface, _dhat, /*dmin=*/0, _broad_phase_method);
		cached_displaced_surface = displaced_surface;
	}

	bool ShapeProblem::is_intersection_free(const TVector &x)
	{
		if (!has_collision)
			return true;

		Eigen::MatrixXd V;
		x_to_param(x, V_rest, V);

		return !ipc::has_intersections(collision_mesh, collision_mesh.vertices(V));
	}

	bool ShapeProblem::is_step_collision_free(const TVector &x0, const TVector &x1)
	{
		if (!has_collision)
			return true;

		Eigen::MatrixXd V0, V1;
		x_to_param(x0, V_rest, V0);
		x_to_param(x1, V_rest, V1);

		// Skip CCD if the displacement is zero.
		if ((V1 - V0).lpNorm<Eigen::Infinity>() == 0.0)
		{
			// Assumes initially intersection-free
			assert(is_intersection_free(x0));
			return true;
		}

		bool is_valid;
		if (_use_cached_candidates)
			is_valid = ipc::is_step_collision_free(
				_candidates, collision_mesh,
				collision_mesh.vertices(V0),
				collision_mesh.vertices(V1),
				_ccd_tolerance, _ccd_max_iterations);
		else
			is_valid = ipc::is_step_collision_free(
				collision_mesh,
				collision_mesh.vertices(V0),
				collision_mesh.vertices(V1),
				ipc::BroadPhaseMethod::HASH_GRID, _ccd_tolerance, _ccd_max_iterations);

		return is_valid;
	}

	double ShapeProblem::target_value(const TVector &x)
	{
		if (mesh_flipped)
			return std::nan("");
		return j->energy(state) * target_weight;
	}

	double ShapeProblem::volume_value(const TVector &x)
	{
		if (has_volume_constraint && !mesh_flipped)
			return j_volume->energy(state) * volume_params["weight"].get<double>();
		else
			return 0.;
	}

	double ShapeProblem::smooth_value(const TVector &x)
	{
		if (!has_boundary_smoothing)
			return 0.;

		Eigen::MatrixXd V;
		x_to_param(x, V_rest, V);

		if (boundary_smoothing_params["scale_invariant"].get<bool>())
			return boundary_smoother.weighted_smoothing_energy(V) * boundary_smoothing_params["weight"].get<double>();
		else
			return boundary_smoother.smoothing_energy(V) * boundary_smoothing_params["weight"].get<double>();
	}

	double ShapeProblem::value(const TVector &x)
	{
		if (std::isnan(cur_val))
		{
			double target_val, volume_val, smooth_val, barrier_val;
			target_val = target_value(x);
			volume_val = volume_value(x);
			smooth_val = smooth_value(x);
			barrier_val = barrier_energy(x);
			logger().debug("target = {}, vol = {}, smooth = {}, barrier = {}", target_val, volume_val, smooth_val, barrier_val);
			cur_val = target_val + volume_val + smooth_val + barrier_val;
		}
		return cur_val;
	}

	void ShapeProblem::target_gradient(const TVector &x, TVector &gradv)
	{
		if (mesh_flipped)
		{
			gradv.setZero(x.size());
			return;
		}
		dparam_to_dx(gradv, j->gradient(state, "shape") * target_weight);
	}

	void ShapeProblem::smooth_gradient(const TVector &x, TVector &gradv)
	{
		if (!has_boundary_smoothing || mesh_flipped)
		{
			gradv.setZero(x.size());
			return;
		}
		Eigen::MatrixXd V;
		x_to_param(x, V_rest, V);
		TVector grad;
		if (boundary_smoothing_params["scale_invariant"].get<bool>())
			boundary_smoother.weighted_smoothing_grad(V, grad);
		else
			boundary_smoother.smoothing_grad(V, grad);
		grad *= boundary_smoothing_params["weight"];

		dparam_to_dx(gradv, grad);
	}

	void ShapeProblem::volume_gradient(const TVector &x, TVector &gradv)
	{
		gradv.setZero(x.size());
		if (!has_volume_constraint || mesh_flipped)
			return;

		dparam_to_dx(gradv, j_volume->gradient(state, "shape") * volume_params["weight"]);
	}

	double ShapeProblem::barrier_energy(const TVector &x)
	{
		if (!has_collision || mesh_flipped)
			return 0;

		Eigen::MatrixXd V;
		x_to_param(x, V_rest, V);

		return _barrier_stiffness * ipc::compute_barrier_potential(collision_mesh, collision_mesh.vertices(V), _constraint_set, _dhat);
	}

	void ShapeProblem::barrier_gradient(const TVector &x, TVector &gradv)
	{
		if (!has_collision || mesh_flipped)
		{
			gradv.setZero(x.size());
			return;
		}

		Eigen::MatrixXd V;
		x_to_param(x, V_rest, V);

		Eigen::MatrixXd displaced_surface = collision_mesh.vertices(V);
		TVector grad;
		grad = ipc::compute_barrier_potential_gradient(
			collision_mesh, displaced_surface, _constraint_set, _dhat);
		grad = collision_mesh.to_full_dof(grad);

		grad *= _barrier_stiffness;

		dparam_to_dx(gradv, grad);
	}

	void ShapeProblem::gradient(const TVector &x, TVector &gradv)
	{
		if (cur_grad.size() == 0)
		{
			Eigen::VectorXd grad_target, grad_smoothing, grad_volume, grad_barrier;
			target_gradient(x, grad_target);
			smooth_gradient(x, grad_smoothing);
			volume_gradient(x, grad_volume);
			barrier_gradient(x, grad_barrier);
			logger().debug("‖∇ target‖ = {}, ‖∇ vol‖ = {}, ‖∇ smooth‖ = {}, ‖∇ barrier‖ = {}", grad_target.norm(), grad_volume.norm(), grad_smoothing.norm(), grad_barrier.norm());
			cur_grad = grad_target + grad_volume + grad_smoothing + grad_barrier;
		}

		gradv = cur_grad;
	}

	void ShapeProblem::smoothing(const TVector &x, TVector &new_x)
	{
		Eigen::MatrixXd V, new_V;
		x_to_param(x, V_rest, V);

		double rate = 2.;
		bool good_enough = false;
		Eigen::MatrixXd boundary_constraints = Eigen::MatrixXd::Zero(state.boundary_gnodes.size(), dim);

		do
		{
			rate /= 2;
			logger().trace("Try SLIM with step size {}", rate);
			TVector tmp_x = (1. - rate) * x + rate * new_x;
			Eigen::MatrixXd tmp_V;
			x_to_param(tmp_x, V_rest, tmp_V);
			for (int b = 0; b < state.boundary_gnodes.size(); ++b)
				boundary_constraints.row(b) = tmp_V.block(state.boundary_gnodes[b], 0, 1, dim);

			good_enough = internal_smoothing(V, elements, state.boundary_gnodes, boundary_constraints, slim_params, new_V);
		} while (!good_enough || is_flipped(new_V, elements));

		logger().debug("SLIM succeeds with step size {}", rate);

		V_rest = new_V;
		param_to_x(new_x, new_V);
		solution_changed(new_x);
	}

	bool ShapeProblem::is_step_valid(const TVector &x0, const TVector &x1)
	{
		Eigen::MatrixXd V1;
		x_to_param(x1, V_rest, V1);
		if (is_flipped(V1, elements))
			return false;

		return true;
	}

	double ShapeProblem::max_step_size(const TVector &x0, const TVector &x1)
	{
		if (!has_collision)
			return 1;

		Eigen::MatrixXd V0, V1;
		x_to_param(x0, V_rest, V0);
		x_to_param(x1, V_rest, V1);

		double max_step = 1;
		assert(!is_flipped(V0, elements));
		while (is_flipped(V0 + max_step * (V1 - V0), elements))
			max_step /= 2.;

		// Extract surface only
		V0 = collision_mesh.vertices(V0);
		V1 = collision_mesh.vertices(V1);

		auto Vmid = V0 + max_step * (V1 - V0);
		if (_use_cached_candidates)
			max_step *= ipc::compute_collision_free_stepsize(
				_candidates, collision_mesh, V0, Vmid,
				_ccd_tolerance, _ccd_max_iterations);
		else
			max_step *= ipc::compute_collision_free_stepsize(
				collision_mesh, V0, Vmid,
				_broad_phase_method, _ccd_tolerance, _ccd_max_iterations);
		// polyfem::logger().trace("best step {}", max_step);

		return max_step;
	}

	void ShapeProblem::line_search_begin(const TVector &x0, const TVector &x1)
	{
		OptimizationProblem::line_search_begin(x0, x1);

		x_at_ls_begin = x0;
		if (!state.problem->is_time_dependent())
			sol_at_ls_begin = state.diff_cached[0].u;

		if (!has_collision)
			return;

		Eigen::MatrixXd V0, V1;
		x_to_param(x0, V_rest, V0);
		x_to_param(x1, V_rest, V1);

		ipc::construct_collision_candidates(
			collision_mesh,
			collision_mesh.vertices(V0),
			collision_mesh.vertices(V1),
			_candidates,
			/*inflation_radius=*/_dhat / 1.99, // divide by 1.99 instead of 2 to be conservative
			_broad_phase_method);

		_use_cached_candidates = true;
	}

	void ShapeProblem::line_search_end(bool failed)
	{
		_candidates.clear();
		_use_cached_candidates = false;
	}

	void ShapeProblem::post_step(const int iter_num, const TVector &x0)
	{
		if (boundary_smoothing_params.contains("adjust_weight_period"))
			if (iter % boundary_smoothing_params["adjust_weight_period"].get<int>() == 0 && iter > 0)
			{
				TVector target_grad, smooth_grad;
				target_gradient(x0, target_grad);
				smooth_gradient(x0, smooth_grad);
				boundary_smoothing_params["weight"] = target_grad.norm() / smooth_grad.norm() * boundary_smoothing_params["adjustment_coeff"].get<double>() * boundary_smoothing_params["weight"].get<double>();

				logger().info("update smoothing weight to {}", boundary_smoothing_params["weight"]);
			}

		iter++;

		if (has_collision)
		{
			Eigen::MatrixXd V;
			x_to_param(x0, V_rest, V);
			Eigen::MatrixXd displaced_surface = state.collision_mesh.vertices(V);

			const double dist_sqr = ipc::compute_minimum_distance(collision_mesh, displaced_surface, _constraint_set);
			polyfem::logger().trace("min_dist {}", sqrt(dist_sqr));

			_prev_distance = dist_sqr;
		}
	}

	bool ShapeProblem::solution_changed_pre(const TVector &newX)
	{
		Eigen::MatrixXd V;
		x_to_param(newX, V_rest, V);
		mesh_flipped = is_flipped(V, elements);
		if (mesh_flipped)
		{
			logger().debug("Mesh Flipped!");
			return false;
		}

		state.set_v(V);
		return true;
	}

	void ShapeProblem::solution_changed_post(const TVector &newX)
	{
		OptimizationProblem::solution_changed_post(newX);

		if (!has_collision || mesh_flipped)
			return;

		Eigen::MatrixXd V;
		x_to_param(newX, V_rest, V);
		update_constraint_set(collision_mesh.vertices(V));
	}

	bool ShapeProblem::remesh(TVector &x)
	{
		// quality check
		Eigen::MatrixXd V;
		Eigen::MatrixXi F;
		state.get_vf(V, F);
		Eigen::VectorXd quality;
		scaled_jacobian(V, F, quality);

		double min_quality = quality.minCoeff();
		double avg_quality = quality.sum() / quality.size();
		logger().debug("Mesh worst quality: {}, avg quality: {}", min_quality, avg_quality);

		static int no_remesh_iter = 1;

		bool should_remesh = false;

		json remesh_args = shape_params["remesh"];
		if (remesh_args.size() == 0)
			return true;

		if (min_quality < remesh_args["tolerance"].get<double>())
		{
			should_remesh = true;
			logger().debug("Remesh due to bad quality...");
		}

		if (no_remesh_iter % remesh_args["period"].get<int>() == 0)
		{
			should_remesh = true;
			logger().debug("Remesh every {} iter...", remesh_args["period"].get<int>());
		}

		if (!should_remesh)
		{
			no_remesh_iter++;
			return false;
		}

		no_remesh_iter = 1;

		// logger().info("Remeshing ...");

		const auto &gbases = state.geom_bases();

		if (V.cols() < 3)
		{
			V.conservativeResize(V.rows(), 3);
			V.col(2).setZero();
		}

		if (is_flipped(V, F))
		{
			logger().error("Mesh flippped during remeshing!");
			exit(0);
		}

		std::set<int> optimize_body_ids;
		if (shape_params.contains("volume_selection"))
		{
			for (int i : shape_params["volume_selection"])
				optimize_body_ids.insert(i);
		}
		else
		{
			for (auto &geometry : state.args["geometry"])
			{
				if (geometry["volume_selection"].is_number_integer())
					optimize_body_ids.insert(geometry["volume_selection"].get<int>());
				else
					logger().error("Remeshing doesn't support single geometry with multiply volume selections!");
			}
		}

		{
			for (int body_id : optimize_body_ids)
			{
				// build submesh
				Eigen::VectorXi vertex_mask, cell_mask, vertex_map(V.rows());
				vertex_mask.setZero(V.rows());
				cell_mask.setZero(F.rows());
				vertex_map.setConstant(-1);
				for (int e = 0; e < gbases.size(); e++)
				{
					if (state.mesh->get_body_id(e) != body_id)
						continue;
					cell_mask[e] = 1;
				}
				for (int f = 0; f < F.rows(); f++)
					for (int v = 0; v < F.cols(); v++)
						vertex_mask[F(f, v)] = 1;
				int idx = 0;
				for (int v = 0; v < V.rows(); v++)
					if (vertex_mask[v])
					{
						vertex_map[v] = idx;
						idx++;
					}

				Eigen::MatrixXd Vm(vertex_mask.sum(), 3);
				Eigen::MatrixXi Fm(cell_mask.sum(), state.mesh->dimension() + 1);
				for (int v = 0; v < V.rows(); v++)
					if (vertex_map[v] >= 0)
						Vm.row(vertex_map[v]) = V.row(v);
				idx = 0;
				for (int f = 0; f < F.rows(); f++)
					if (cell_mask[f])
					{
						for (int v = 0; v < F.cols(); v++)
							Fm(idx, v) = vertex_map[F(f, v)];
						idx++;
					}

				std::string before_remesh_path, after_remesh_path;
				if (!state.mesh->is_volume())
				{
					before_remesh_path = state.resolve_output_path(fmt::format("before_remesh_iter{:d}_mesh{:d}.obj", iter, body_id));
					after_remesh_path = state.resolve_output_path(fmt::format("after_remesh_iter{:d}_mesh{:d}.msh", iter, body_id));

					igl::write_triangle_mesh(before_remesh_path, Vm, Fm);

					double target_length = igl::avg_edge_length(Vm, Fm);

					std::string command = "python remesh.py " + before_remesh_path + " " + after_remesh_path;
					int return_val = system(command.c_str());
					if (return_val == 0)
						logger().info("remesh command \"{}\" returns {}", command, return_val);
					else
					{
						logger().error("remesh command \"{}\" returns {}", command, return_val);
						return false;
					}
				}
				else
				{
					before_remesh_path = state.resolve_output_path(fmt::format("before_remesh_iter{:d}_mesh{:d}.mesh", iter, body_id));
					after_remesh_path = state.resolve_output_path(fmt::format("after_remesh_iter{:d}_mesh{:d}.msh", iter, body_id));

					igl::writeMESH(before_remesh_path, Vm, Fm, Eigen::MatrixXi());

					auto tmp_before_remesh_path = utils::StringUtils::replace_ext(before_remesh_path, "msh");

					int return_val = system(("gmsh " + before_remesh_path + " -save -format msh22 -o " + tmp_before_remesh_path).c_str());
					if (return_val != 0)
					{
						logger().error("gmsh command \"{}\" returns {}", "gmsh " + before_remesh_path + " -save -format msh22 -o " + tmp_before_remesh_path, return_val);
						return false;
					}

					{
						std::string command = remesh_args["remesh_exe"].template get<std::string>() + " " + tmp_before_remesh_path + " " + after_remesh_path + " -j 10";
						return_val = system(command.c_str());
						if (return_val == 0)
							logger().info("remesh command \"{}\" returns {}", command, return_val);
						else
						{
							logger().error("remesh command \"{}\" returns {}", command, return_val);
							return false;
						}
					}
				}

				// modify json
				bool flag = false;
				for (int m = 0; m < state.in_args["geometry"].get<std::vector<json>>().size(); m++)
				{
					if (state.in_args["geometry"][m]["volume_selection"].get<int>() != body_id)
						continue;
					if (!flag)
						flag = true;
					else
					{
						logger().error("Multiple meshes found with same body id!");
						return false;
					}
					state.in_args["geometry"][m]["transformation"]["skip"] = true;
					state.in_args["geometry"][m]["mesh"] = after_remesh_path;
				}
			}
		}

		// initialize things after remeshing
		state.mesh.reset();
		state.mesh = nullptr;
		state.assembler.update_lame_params(Eigen::MatrixXd(), Eigen::MatrixXd());

		json in_args = state.in_args;
		for (auto &geo : in_args["geometry"])
			if (geo.contains("transformation"))
				geo.erase("transformation");
		std::cout << in_args << std::endl;
		state.init(in_args, false);

		state.load_mesh();
		state.stats.compute_mesh_stats(*state.mesh);
		state.build_basis();

		state.get_vf(V_rest, elements);
		param_to_x(x, V_rest);

		Eigen::MatrixXd boundary_nodes_pos;
		state.build_collision_mesh(boundary_nodes_pos, collision_mesh, state.n_geom_bases, gbases);

		build_fixed_nodes();
		build_tied_nodes();

		cur_grad.resize(0);
		cur_val = std::nan("");

		sol_at_ls_begin.resize(0, 0);
		x_at_ls_begin.resize(0);

		boundary_smoother.build_laplacian(state.n_geom_bases, state.mesh->dimension(), collision_mesh.edges(), state.boundary_gnodes, fixed_nodes);

		logger().info("Remeshing finished!");

		state.get_vf(V, F);
		scaled_jacobian(V, F, quality);

		min_quality = quality.minCoeff();
		avg_quality = quality.sum() / quality.size();
		logger().debug("Mesh worst quality: {}, avg quality: {}", min_quality, avg_quality);

		return true;
	}

	void ShapeProblem::build_tied_nodes()
	{
		const double correspondence_threshold = shape_params.value("correspondence_threshold", 1e-8);
		const double displace_dist = shape_params.value("displace_dist", 1e-4);

		tied_nodes.clear();
		tied_nodes_mask.assign(V_rest.rows(), false);
		for (int i = 0; i < V_rest.rows(); i++)
		{
			for (int j = 0; j < i; j++)
			{
				if ((V_rest.row(i) - V_rest.row(j)).norm() < correspondence_threshold)
				{
					tied_nodes.push_back(std::array<int, 2>({{i, j}}));
					tied_nodes_mask[i] = true;
					tied_nodes_mask[j] = true;
					logger().trace("Tie {} and {}", i, j);
					break;
				}
			}
		}

		if (tied_nodes.size() == 0)
			return;

		assembler::ElementAssemblyValues vals;
		Eigen::MatrixXd uv, samples, gtmp, rhs_fun;
		Eigen::VectorXi global_primitive_ids;
		Eigen::MatrixXd points, normals;
		Eigen::VectorXd weights;
		const auto &gbases = state.geom_bases();

		Eigen::VectorXd vertex_perturbation;
		vertex_perturbation.setZero(state.n_geom_bases * dim, 1);

		Eigen::VectorXi n_shared_edges;
		n_shared_edges.setZero(state.n_geom_bases);
		for (const auto &lb : state.total_local_boundary)
		{
			const int e = lb.element_id();
			bool has_samples = utils::BoundarySampler::boundary_quadrature(lb, 1, *state.mesh, false, uv, points, normals, weights, global_primitive_ids);

			if (!has_samples)
				continue;

			const basis::ElementBases &gbs = gbases[e];

			vals.compute(e, state.mesh->is_volume(), points, gbs, gbs);

			const int n_quad_pts = weights.size() / lb.size();
			for (int n = 0; n < vals.jac_it.size(); ++n)
			{
				normals.row(n) = normals.row(n) * vals.jac_it[n];
				normals.row(n).normalize();
			}

			for (int i = 0; i < lb.size(); ++i)
			{
				const int primitive_global_id = lb.global_primitive_id(i);
				const auto nodes = gbases[e].local_nodes_for_primitive(primitive_global_id, *state.mesh);

				for (long n = 0; n < nodes.size(); ++n)
				{
					const assembler::AssemblyValues &v = vals.basis_values[nodes(n)];
					assert(v.global.size() == 1);

					if (tied_nodes_mask[v.global[0].index])
					{
						vertex_perturbation(Eigen::seqN(v.global[0].index * dim, dim)) -= normals(n_quad_pts * i, Eigen::seqN(0, dim)).transpose();
						n_shared_edges(v.global[0].index) += 1;
					}
				}
			}
		}
		for (int i = 0; i < n_shared_edges.size(); i++)
			if (n_shared_edges(i) > 1)
				vertex_perturbation(Eigen::seqN(i * dim, dim)) *= displace_dist / vertex_perturbation(Eigen::seqN(i * dim, dim)).norm();
		state.pre_sol = state.down_sampling_mat.transpose() * vertex_perturbation;
	}

	void ShapeProblem::build_fixed_nodes()
	{
		fixed_nodes.clear();
		const auto &gbases = state.geom_bases();

		Eigen::MatrixXd V;
		state.get_vf(V, elements);
		V.conservativeResize(V.rows(), state.mesh->dimension());

		// fix certain object
		std::set<int> optimize_body_ids;
		std::set<int> optimize_boundary_ids;
		if (shape_params["volume_selection"].size() > 0)
		{
			for (int i : shape_params["volume_selection"])
				optimize_body_ids.insert(i);

			for (int e = 0; e < state.bases.size(); e++)
			{
				const int body_id = state.mesh->get_body_id(e);
				if (!optimize_body_ids.count(body_id))
					for (const auto &gbs : gbases[e].bases)
						for (const auto &g : gbs.global())
							fixed_nodes.insert(g.index);
			}
		}
		else if (shape_params["surface_selection"].size() > 0)
		{
			for (int i : shape_params["surface_selection"])
				optimize_boundary_ids.insert(i);

			for (const auto &lb : state.total_local_boundary)
			{
				const int e = lb.element_id();
				for (int i = 0; i < lb.size(); ++i)
				{
					const int primitive_global_id = lb.global_primitive_id(i);
					const auto nodes = state.bases[e].local_nodes_for_primitive(primitive_global_id, *state.mesh);

					if (!optimize_boundary_ids.count(state.mesh->get_boundary_id(primitive_global_id)))
					{
						for (long n = 0; n < nodes.size(); ++n)
						{
							fixed_nodes.insert(state.bases[e].bases[nodes(n)].global()[0].index);
						}
					}
					else
					{
						for (long n = 0; n < nodes.size(); ++n)
						{
							if (optimization_boundary_to_node.count(state.mesh->get_boundary_id(primitive_global_id)) != 0)
							{
								if (std::count(optimization_boundary_to_node[state.mesh->get_boundary_id(primitive_global_id)].begin(), optimization_boundary_to_node[state.mesh->get_boundary_id(primitive_global_id)].end(), state.bases[e].bases[nodes(n)].global()[0].index) == 0)
								{
									optimization_boundary_to_node[state.mesh->get_boundary_id(primitive_global_id)].push_back(state.bases[e].bases[nodes(n)].global()[0].index);
								}
							}
							else
							{
								optimization_boundary_to_node[state.mesh->get_boundary_id(primitive_global_id)] = {state.bases[e].bases[nodes(n)].global()[0].index};
							}
						}
					}
				}
			}
		}
		else
			logger().info("No optimization body or boundary specified, optimize shape of every mesh...");

		// fix dirichlet bc
		if (!shape_params.contains("fix_dirichlet") || shape_params["fix_dirichlet"].get<bool>())
		{
			logger().info("Fix position of Dirichlet boundary nodes.");
			for (const auto &lb : state.local_boundary)
			{
				for (int i = 0; i < lb.size(); ++i)
				{
					const int e = lb.element_id();
					const int primitive_g_id = lb.global_primitive_id(i);
					const int tag = state.mesh->get_boundary_id(primitive_g_id);
					const auto nodes = gbases[e].local_nodes_for_primitive(primitive_g_id, *(state.mesh));

					if (tag <= 0)
						continue;

					for (long n = 0; n < nodes.size(); ++n)
					{
						assert(gbases[e].bases[nodes(n)].global().size() == 1);
						fixed_nodes.insert(gbases[e].bases[nodes(n)].global()[0].index);
					}
				}
			}
		}

		// fix neumann bc
		logger().info("Fix position of nonzero Neumann boundary nodes.");
		for (const auto &lb : state.local_neumann_boundary)
		{
			for (int i = 0; i < lb.size(); ++i)
			{
				const int e = lb.element_id();
				const int primitive_g_id = lb.global_primitive_id(i);
				const int tag = state.mesh->get_boundary_id(primitive_g_id);
				const auto nodes = gbases[e].local_nodes_for_primitive(primitive_g_id, *(state.mesh));

				if (tag <= 0)
					continue;

				for (long n = 0; n < nodes.size(); ++n)
				{
					assert(gbases[e].bases[nodes(n)].global().size() == 1);
					fixed_nodes.insert(gbases[e].bases[nodes(n)].global()[0].index);
				}
			}
		}

		// fix contact area, need threshold
		if (shape_params.contains("fix_contact_surface") && shape_params["fix_contact_surface"].get<bool>())
		{
			const double threshold = shape_params["fix_contact_surface_tol"].get<double>();
			logger().info("Fix position of boundary nodes in contact.");

			ipc::Constraints contact_set;
			contact_set.build(collision_mesh, collision_mesh.vertices(V), threshold);

			for (int c = 0; c < contact_set.ee_constraints.size(); c++)
			{
				const auto &constraint = contact_set.ee_constraints[c];

				if (constraint.compute_distance(collision_mesh.vertices(V), collision_mesh.edges(), collision_mesh.faces()) >= threshold)
					continue;

				fixed_nodes.insert(collision_mesh.to_full_vertex_id(constraint.vertex_indices(collision_mesh.edges(), collision_mesh.faces())[0]));
				fixed_nodes.insert(collision_mesh.to_full_vertex_id(constraint.vertex_indices(collision_mesh.edges(), collision_mesh.faces())[1]));
			}

			contact_set.ee_constraints.clear();

			for (int c = 0; c < contact_set.size(); c++)
			{
				const auto &constraint = contact_set[c];

				if (constraint.compute_distance(collision_mesh.vertices(V), collision_mesh.edges(), collision_mesh.faces()) >= threshold)
					continue;

				fixed_nodes.insert(collision_mesh.to_full_vertex_id(constraint.vertex_indices(collision_mesh.edges(), collision_mesh.faces())[0]));
			}
		}

		// fix nodes based on problem setting
		if (opt_params.contains("name") && opt_params["name"].get<std::string>() == "door")
		{
			for (int i = 0; i < V.rows(); i++)
			{
				// the boundary that touches the door
				// if (V(i, 0) > -0.883353 && V(i, 0) < 0.177471 && V(i, 1) < 0.824529)
				// 	fixed_nodes.insert(i);
				// the handle of hook
				if (V(i, 0) > 0.7647)
					fixed_nodes.insert(i);
				// the left end of hook, to not let it degenerate
				// else if (V(i, 1) < 0.471 && V(i, 0) < -0.88 && V(i, 0) > -0.95)
				// 	fixed_nodes.insert(i);
			}
		}

		logger().info("Fixed nodes: {}", fixed_nodes.size());
	}
} // namespace polyfem
