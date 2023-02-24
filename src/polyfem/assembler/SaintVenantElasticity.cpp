// #define EIGEN_STACK_ALLOCATION_LIMIT 0

#include "SaintVenantElasticity.hpp"

#include <polyfem/basis/Basis.hpp>
#include <polyfem/autogen/auto_elasticity_rhs.hpp>

#include <igl/Timer.h>

namespace polyfem::assembler
{
	namespace
	{
		template <class Matrix>
		Matrix strain_from_disp_grad(const Matrix &disp_grad)
		{
			// Matrix mat =  (disp_grad + disp_grad.transpose());
			Matrix mat = (disp_grad.transpose() * disp_grad + disp_grad + disp_grad.transpose());

			for (int i = 0; i < mat.size(); ++i)
				mat(i) *= 0.5;

			return mat;
		}

		// template<int dim>
		// Eigen::Matrix<double, dim, dim> strain(const Eigen::MatrixXd &grad, const Eigen::MatrixXd &jac_it, int k, int coo)
		// {
		// 	Eigen::Matrix<double, dim, dim> jac;
		// 	jac.setZero();
		// 	jac.row(coo) = grad.row(k);
		// 	jac = jac*jac_it;

		// 	return strain_from_disp_grad(jac);
		// }
	} // namespace

	SaintVenantElasticity::SaintVenantElasticity()
	{
	}

	void SaintVenantElasticity::add_multimaterial(const int index, const json &params)
	{
		assert(size_ == 2 || size_ == 3);

		if (!params.contains("elasticity_tensor"))
		{
			if (params.count("young"))
			{
				if (params["young"].is_number() && params["nu"].is_number())
					elasticity_tensor_.set_from_young_poisson(params["young"], params["nu"]);
			}
			else if (params.count("E"))
			{
				if (params["E"].is_number() && params["nu"].is_number())
					elasticity_tensor_.set_from_young_poisson(params["E"], params["nu"]);
			}
			else if (params.count("lambda"))
			{
				if (params["lambda"].is_number() && params["mu"].is_number())
					elasticity_tensor_.set_from_lambda_mu(params["lambda"], params["mu"]);
			}
		}
		else
		{
			std::vector<double> entries = params["elasticity_tensor"];
			elasticity_tensor_.set_from_entries(entries);
		}
	}

	void SaintVenantElasticity::set_size(const int size)
	{
		elasticity_tensor_.resize(size);
		size_ = size;
	}

	template <typename T, unsigned long N>
	T SaintVenantElasticity::stress(const std::array<T, N> &strain, const int j) const
	{
		T res = elasticity_tensor_(j, 0) * strain[0];

		for (unsigned long k = 1; k < N; ++k)
			res += elasticity_tensor_(j, k) * strain[k];

		return res;
	}

	Eigen::Matrix<double, Eigen::Dynamic, 1, 0, 3, 1>
	SaintVenantElasticity::compute_rhs(const AutodiffHessianPt &pt) const
	{
		assert(pt.size() == size());
		Eigen::Matrix<double, Eigen::Dynamic, 1, 0, 3, 1> res;

		if (size() == 2)
			autogen::saint_venant_2d_function(pt, elasticity_tensor_, res);
		else if (size() == 3)
			autogen::saint_venant_3d_function(pt, elasticity_tensor_, res);
		else
			assert(false);

		return res;
	}

	Eigen::VectorXd
	SaintVenantElasticity::assemble_grad(const NonLinearAssemblerData &data) const
	{
		// igl::Timer time; time.start();

		const int n_bases = data.vals.basis_values.size();

		return polyfem::gradient_from_energy(
			size(), n_bases, data,
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 6, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 8, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 12, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 18, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 24, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 30, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 60, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, 81, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, Eigen::Dynamic, 1, 0, SMALL_N, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::Matrix<double, Eigen::Dynamic, 1, 0, BIG_N, 1>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar1<double, Eigen::VectorXd>>(data); });
	}

	Eigen::MatrixXd
	SaintVenantElasticity::assemble_hessian(const NonLinearAssemblerData &data) const
	{
		// igl::Timer time; time.start();

		const int n_bases = data.vals.basis_values.size();
		return polyfem::hessian_from_energy(
			size(), n_bases, data,
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 6, 1>, Eigen::Matrix<double, 6, 6>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 8, 1>, Eigen::Matrix<double, 8, 8>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 12, 1>, Eigen::Matrix<double, 12, 12>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 18, 1>, Eigen::Matrix<double, 18, 18>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 24, 1>, Eigen::Matrix<double, 24, 24>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 30, 1>, Eigen::Matrix<double, 30, 30>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 60, 1>, Eigen::Matrix<double, 60, 60>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, 81, 1>, Eigen::Matrix<double, 81, 81>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::Matrix<double, Eigen::Dynamic, 1, 0, SMALL_N, 1>, Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, 0, SMALL_N, SMALL_N>>>(data); },
			[&](const NonLinearAssemblerData &data) { return compute_energy_aux<DScalar2<double, Eigen::VectorXd, Eigen::MatrixXd>>(data); });
	}

	void SaintVenantElasticity::compute_cauchy_stress_tensor(const int el_id, const basis::ElementBases &bs, const basis::ElementBases &gbs, const Eigen::MatrixXd &local_pts, const Eigen::MatrixXd &displacement, Eigen::MatrixXd &stresses) const
	{
		assign_stress_tensor(el_id, bs, gbs, local_pts, displacement, size() * size(), false, stresses, [&](const Eigen::MatrixXd &stress) {
			Eigen::MatrixXd tmp = stress;
			auto a = Eigen::Map<Eigen::MatrixXd>(tmp.data(), 1, size() * size());
			return Eigen::MatrixXd(a);
		});
	}
	void SaintVenantElasticity::compute_pk1_stress_tensor(const int el_id, const basis::ElementBases &bs, const basis::ElementBases &gbs, const Eigen::MatrixXd &local_pts, const Eigen::MatrixXd &displacement, Eigen::MatrixXd &stresses) const
	{
		assign_stress_tensor(el_id, bs, gbs, local_pts, displacement, size() * size(), true, stresses, [&](const Eigen::MatrixXd &stress) {
			Eigen::MatrixXd tmp = stress;
			auto a = Eigen::Map<Eigen::MatrixXd>(tmp.data(), 1, size() * size());
			return Eigen::MatrixXd(a);
		});
	}

	void SaintVenantElasticity::compute_von_mises_stresses(const int el_id, const basis::ElementBases &bs, const basis::ElementBases &gbs, const Eigen::MatrixXd &local_pts, const Eigen::MatrixXd &displacement, Eigen::MatrixXd &stresses) const
	{
		assign_stress_tensor(el_id, bs, gbs, local_pts, displacement, 1, false, stresses, [&](const Eigen::MatrixXd &stress) {
			Eigen::Matrix<double, 1, 1> res;
			res.setConstant(von_mises_stress_for_stress_tensor(stress));
			return res;
		});
	}

	void SaintVenantElasticity::assign_stress_tensor(const int el_id, const basis::ElementBases &bs, const basis::ElementBases &gbs, const Eigen::MatrixXd &local_pts, const Eigen::MatrixXd &displacement, const int all_size, const bool pk1, Eigen::MatrixXd &all, const std::function<Eigen::MatrixXd(const Eigen::MatrixXd &)> &fun) const
	{
		Eigen::MatrixXd displacement_grad(size(), size());

		assert(displacement.cols() == 1);

		all.resize(local_pts.rows(), all_size);

		ElementAssemblyValues vals;
		vals.compute(el_id, size() == 3, local_pts, bs, gbs);

		for (long p = 0; p < local_pts.rows(); ++p)
		{
			compute_diplacement_grad(size(), bs, vals, local_pts, p, displacement, displacement_grad);

			Eigen::MatrixXd strain = strain_from_disp_grad(displacement_grad);
			Eigen::MatrixXd stress_tensor(size(), size());

			if (size() == 2)
			{
				std::array<double, 3> eps;
				eps[0] = strain(0, 0);
				eps[1] = strain(1, 1);
				eps[2] = 2 * strain(0, 1);

				stress_tensor << stress(eps, 0), stress(eps, 2),
					stress(eps, 2), stress(eps, 1);
			}
			else
			{
				std::array<double, 6> eps;
				eps[0] = strain(0, 0);
				eps[1] = strain(1, 1);
				eps[2] = strain(2, 2);
				eps[3] = 2 * strain(1, 2);
				eps[4] = 2 * strain(0, 2);
				eps[5] = 2 * strain(0, 1);

				stress_tensor << stress(eps, 0), stress(eps, 5), stress(eps, 4),
					stress(eps, 5), stress(eps, 1), stress(eps, 3),
					stress(eps, 4), stress(eps, 3), stress(eps, 2);
			}

			stress_tensor = (Eigen::MatrixXd::Identity(size(), size()) + displacement_grad) * stress_tensor;
			if (pk1)
				stress_tensor = pk1_from_cauchy(stress_tensor, displacement_grad + Eigen::MatrixXd::Identity(size(), size()));

			all.row(p) = fun(stress_tensor);
		}
	}

	double SaintVenantElasticity::compute_energy(const NonLinearAssemblerData &data) const
	{
		return compute_energy_aux<double>(data);
	}

	// Compute \int \sigma : E
	template <typename T>
	T SaintVenantElasticity::compute_energy_aux(const NonLinearAssemblerData &data) const
	{
		typedef Eigen::Matrix<T, Eigen::Dynamic, 1> AutoDiffVect;
		typedef Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic, 0, 3, 3> AutoDiffGradMat;

		AutoDiffVect local_disp;
		get_local_disp(data, size(), local_disp);

		AutoDiffGradMat disp_grad(size(), size());

		T energy = T(0.0);

		const int n_pts = data.da.size();
		for (long p = 0; p < n_pts; ++p)
		{
			compute_disp_grad_at_quad(data, local_disp, p, size(), disp_grad);

			AutoDiffGradMat strain = strain_from_disp_grad(disp_grad);
			AutoDiffGradMat stress_tensor(size(), size());

			if (size() == 2)
			{
				std::array<T, 3> eps;
				eps[0] = strain(0, 0);
				eps[1] = strain(1, 1);
				eps[2] = 2 * strain(0, 1);

				stress_tensor << stress(eps, 0), stress(eps, 2),
					stress(eps, 2), stress(eps, 1);
			}
			else
			{
				std::array<T, 6> eps;
				eps[0] = strain(0, 0);
				eps[1] = strain(1, 1);
				eps[2] = strain(2, 2);
				eps[3] = 2 * strain(1, 2);
				eps[4] = 2 * strain(0, 2);
				eps[5] = 2 * strain(0, 1);

				stress_tensor << stress(eps, 0), stress(eps, 5), stress(eps, 4),
					stress(eps, 5), stress(eps, 1), stress(eps, 3),
					stress(eps, 4), stress(eps, 3), stress(eps, 2);
			}

			energy += (stress_tensor * strain).trace() * data.da(p);
		}

		return energy * 0.5;
	}
} // namespace polyfem::assembler
