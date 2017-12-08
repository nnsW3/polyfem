#include "QuadBasis.hpp"

#include <cassert>

namespace poly_fem
{
	template<typename T>
	Eigen::MatrixXd theta1(T &t)
	{
		return (1-t) * (1-2 * t);
	}

	template<typename T>
	Eigen::MatrixXd theta2(T &t)
	{
		return t * (2 * t - 1);
	}

	template<typename T>
	Eigen::MatrixXd theta3(T &t)
	{
		return 4 * t * (1 - t);
	}



	template<typename T>
	Eigen::MatrixXd dtheta1(T &t)
	{
		return -3+4*t;
	}

	template<typename T>
	Eigen::MatrixXd dtheta2(T &t)
	{
		return -1+4*t;
	}

	template<typename T>
	Eigen::MatrixXd dtheta3(T &t)
	{
		return 4-8*t;
	}

	int QuadBasis::build_bases(const Mesh &mesh, std::vector< std::vector<Basis> > &bases, std::vector< int > &bounday_nodes)
	{
		assert(!mesh.is_volume);

		const int disc_order = 1;

		bases.resize(mesh.els.rows());
		const int n_bases = int(mesh.pts.rows());;

		for(long i = 0; i < mesh.els.rows(); ++i)
		{
			std::vector<Basis> &b=bases[i];
			b.resize(4);

			for(int j = 0; j < 4; ++j)
			{
				const int global_index = mesh.els(i,j);
				b[j].init(global_index, j, mesh.pts.row(global_index));

				b[j].set_basis([disc_order, j](const Eigen::MatrixXd &uv, Eigen::MatrixXd &val) { QuadBasis::basis(disc_order, j,uv, val); });
				b[j].set_grad( [disc_order, j](const Eigen::MatrixXd &uv, Eigen::MatrixXd &val) {  QuadBasis::grad(disc_order, j,uv, val); });
			}
		}

		return n_bases;
	}


	void QuadBasis::basis(const int disc_order, const int local_index, const Eigen::MatrixXd &uv, Eigen::MatrixXd &val)
	{
		switch(disc_order)
		{
			case 1:
			{
				switch(local_index)
				{
					case 0: val = (1-uv.col(0).array())*(1-uv.col(1).array()); break;
					case 1: val = uv.col(0).array()*(1-uv.col(1).array()); break;
					case 2: val = uv.col(0).array()*uv.col(1).array(); break;
					case 3: val = (1-uv.col(0).array())*uv.col(1).array(); break;
					default: assert(false);
				}

				break;
			}

			case 2:
			{
				auto &u = uv.col(0).array();
				auto &v = uv.col(1).array();

				switch(local_index)
				{
					case 0: val = theta1(u).array() * theta1(v).array(); break;
					case 1: val = theta2(u).array() * theta1(v).array(); break;
					case 2: val = theta2(u).array() * theta2(v).array(); break;
					case 3: val = theta1(u).array() * theta2(v).array(); break;
					case 4: val = theta3(u).array() * theta1(v).array(); break;
					case 5: val = theta2(u).array() * theta3(v).array(); break;
					case 6: val = theta3(u).array() * theta2(v).array(); break;
					case 7: val = theta1(u).array() * theta3(v).array(); break;
					case 8: val = theta3(u).array() * theta3(v).array(); break;
					default: assert(false);
				}

				break;
			}
			//No Q3 implemented
			default: assert(false);
		}
	}

	void QuadBasis::grad(const int disc_order, const int local_index, const Eigen::MatrixXd &uv, Eigen::MatrixXd &val)
	{
		val.resize(uv.rows(),2);

		switch(disc_order)
		{
			case 1:
			{
				switch(local_index)
				{
					case 0:
					{
						val.col(0) = -(1-uv.col(1).array());
						val.col(1) = -(1-uv.col(0).array());

						break;
					}
					case 1:
					{
						val.col(0) = (1-uv.col(1).array());
						val.col(1) =   -uv.col(0);

						break;
					}
					case 2:
					{
						val.col(0) = uv.col(1);
						val.col(1) = uv.col(0);

						break;
					}
					case 3:
					{
						val.col(0) = -uv.col(1);
						val.col(1) = 1-uv.col(0).array();

						break;
					}

					default: assert(false);
				}

				break;
			}

			case 2:
			{
				auto u=uv.col(0).array();
				auto v=uv.col(1).array();

				switch(local_index)
				{
					case 0:
					{
						val.col(0) = dtheta1(u).array() * theta1(v).array();
						val.col(1) = theta1(u).array() * dtheta1(v).array();
						break;
					}
					case 1:
					{
						val.col(0) = dtheta2(u).array() * theta1(v).array();
						val.col(1) = theta2(u).array() * dtheta1(v).array();
						break;
					}
					case 2:
					{
						val.col(0) = dtheta2(u).array() * theta2(v).array();
						val.col(1) = theta2(u).array() * dtheta2(v).array();
						break;
					}
					case 3:
					{
						val.col(0) = dtheta1(u).array() * theta2(v).array();
						val.col(1) = theta1(u).array() * dtheta2(v).array();
						break;
					}
					case 4:
					{
						val.col(0) = dtheta3(u).array() * theta1(v).array();
						val.col(1) = theta3(u).array() * dtheta1(v).array();
						break;
					}
					case 5:
					{
						val.col(0) = dtheta2(u).array() * theta3(v).array();
						val.col(1) = theta2(u).array() * dtheta3(v).array();
						break;
					}
					case 6:
					{
						val.col(0) = dtheta3(u).array() * theta2(v).array();
						val.col(1) = theta3(u).array() * dtheta2(v).array();
						break;
					}
					case 7:
					{
						val.col(0) = dtheta1(u).array() * theta3(v).array();
						val.col(1) = theta1(u).array() * dtheta3(v).array();
						break;
					}
					case 8:
					{
						val.col(0) = dtheta3(u).array() * theta3(v).array();
						val.col(1) = theta3(u).array() * dtheta3(v).array();
						break;
					}

					default: assert(false);
				}

				break;
			}
			default: assert(false);
		}
	}
}