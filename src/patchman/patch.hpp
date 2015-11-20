//
// Written by Andrea Iob <andrea_iob@hotmail.com>
//
#ifndef __PATCHMAN_PATCH_HPP__
#define __PATCHMAN_PATCH_HPP__

/*! \file */

#include "adaption.hpp"
#include "cell.hpp"
#include "interface.hpp"
#include "output_manager.hpp"
#include "piercedVector.hpp"
#include "node.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pman {

class Patch {

public:
	Patch(const int &id, const int &dimension);

	virtual ~Patch();

	void reset();
	void reset_vertices();
	void reset_cells();
	void reset_interfaces();
	void reset_output();

	const std::vector<Adaption::Info> update(bool trackAdaption = true);

	void mark_cell_for_refinement(const long &id);
	void mark_cell_for_coarsening(const long &id);
	void enable_cell_balancing(const long &id, bool enabled);

	bool is_dirty() const;
	bool is_output_dirty() const;

	int get_id() const;
	int get_dimension() const;
	bool is_three_dimensional() const;

	std::string get_name() const;
	void set_name(std::string name);

	long get_vertex_count() const;
	PiercedVector<Node> &vertices();
	Node &get_vertex(const long &id);

	long get_cell_count() const;
	PiercedVector<Cell> &cells();
	Cell &get_cell(const long &id);

	long get_interface_count() const;
	PiercedVector<Interface> &interfaces();
	Interface &get_interface(const long &id);

	void sort();
	void squeeze();

	void write_mesh();
	void write_mesh(std::string name);
	void write_field(std::string name, int type, std::vector<double> values);
	void write_field(std::string filename, std::string name, int type, std::vector<double> values);
	void write_cell_field(std::string name, std::vector<double> values);
	void write_cell_field(std::string filename, std::string name, std::vector<double> values);
	void write_vertex_field(std::string name, std::vector<double> values);
	void write_vertex_field(std::string filename, std::string name, std::vector<double> values);
	OutputManager & get_output_manager();

	std::array<double, 3> & get_opposite_normal(std::array<double, 3> &normal);

protected:
	PiercedVector<Node> m_vertices;
	PiercedVector<Cell> m_cells;
	PiercedVector<Interface> m_interfaces;

	std::deque<long> m_unusedVertexIds;
	std::deque<long> m_unusedInterfaceIds;
	std::deque<long> m_unusedCellIds;

	long create_vertex();
	long create_vertex(const long &id);
	void delete_vertex(const long &id);

	long create_interface();
	long create_interface(const long &id);
	void delete_interface(const long &id);

	long create_cell(bool internal = true);
	long create_cell(const long &id, bool internal = true);
	void delete_cell(const long &id);

	virtual std::array<double, 3> & _get_opposite_normal(std::array<double, 3> &normal) = 0;
	virtual const std::vector<Adaption::Info> _update(bool trackAdaption) = 0;
	virtual bool _mark_cell_for_refinement(const long &id) = 0;
	virtual bool _mark_cell_for_coarsening(const long &id) = 0;
	virtual bool _enable_cell_balancing(const long &id, bool enabled) = 0;

	void set_dirty(bool dirty);

	void update_output_manager();

private:
	bool m_dirty;
	bool m_dirty_output;

	int m_id;
	int m_dimension;
	std::string m_name;

	vtkSmartPointer<OutputManager> m_output_manager;

	void set_id(int id);
	void set_dimension(int dimension);

};

}

#endif
