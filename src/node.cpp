//
// Written by Andrea Iob <andrea_iob@hotmail.com>
//

#include "node.hpp"

#include <limits>

namespace pman {

/*!
	\class Node

	\brief The Node class defines the nodes.

	Node is class that defines the nodes.
*/

const int Node::NULL_NODE_ID = std::numeric_limits<int>::min();

/*!
	Default constructor.
*/
Node::Node()
{
	set_id(NULL_NODE_ID);
}

/*!
	Creates a new element.
*/
Node::Node(const int &id)
{
	set_id(id);
}

/*!
	Sets the ID of the node.

	\param id the ID of the node
*/
void Node::set_id(const int &id)
{
	m_id = id;
}

/*!
	Gets the ID of the node.

	\return The ID of the node
*/
int Node::get_id() const
{
	return m_id;
}

/*!
	Sets the coordinates of the node.

	\param coords a pointer the coordinates of the node
*/
void Node::set_coords(std::unique_ptr<double[]> coords)
{
	m_coords = std::move(coords);
}

/*!
	Gets the coordinates of the node.

	\return A pointer to the coordinates of the node
*/
double * Node::get_coords() const
{
	return m_coords.get();
}

}
