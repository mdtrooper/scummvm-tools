#include "graph.h"

#include <algorithm>

#include <boost/foreach.hpp>
#include <boost/utility.hpp>

using namespace boost;
using namespace std;

#ifndef foreach
#define foreach BOOST_FOREACH
#endif


void componentVisit(Node *u, Node *head) {
	if (u->_component)
		return;
	u->_component = head;
	foreach (Node *v, u->_in)
		componentVisit(v, head);
}


Node *dominatorIntersect(Node *u, Node *v) {
	while (u != v) {
		while (u->_number < v->_number)
			u = u->_dominator;
		while (v->_number < u->_number)
			v = v->_dominator;
	}
	return u;
}


bool postOrderCompare(Node *a, Node *b) {
	return a->_number < b->_number;
}

list<Node*> inPostOrder(list<Node*> &nodes) {
	list<Node*> ret(nodes);
	ret.sort(postOrderCompare);
	return ret;
}


int orderVisit(Node *u, int number) {
	u->_number = -1;
	foreach (Node *v, u->_out)
		if (!v->_number)
			number = orderVisit(v, number);
	u->_number = ++number;
	return number;
}



ControlFlowGraph::ControlFlowGraph() : _entry() {
}


ControlFlowGraph::~ControlFlowGraph() {
	foreach (Node *u, _nodes)
		delete u;
}


Node *ControlFlowGraph::addNode(list<Instruction*>::iterator first, list<Instruction*>::iterator last) {
	Node* node = new Node;
	_nodes.push_back(node);
	copy(first, last, back_inserter(node->_instructions));
	return node;
}


void ControlFlowGraph::addNodesFromScript(list<Instruction*>::iterator scriptBegin, list<Instruction*>::iterator scriptEnd) {
	Jump *jump;
	for (list<Instruction*>::iterator it = scriptBegin; it != scriptEnd; it++)
		if ((jump = dynamic_cast<Jump*>(*it))) {
			_targets[jump->target()] = 0;
			if (next(it) != scriptEnd)
				_targets[(*next(it))->_addr] = 0;
		}
	list<Instruction*>::iterator first = scriptBegin;
	for (list<Instruction*>::iterator last = scriptBegin; last != scriptEnd; last++) {
		if (next(last) == scriptEnd || contains(_targets, (*next(last))->_addr)) {
			_targets[(*first)->_addr] = addNode(first, next(last));
			first = next(last);
		}
	}
	foreach (Node *node, _nodes) {
		if ((jump = dynamic_cast<Jump*>(node->_instructions.back())))
			addEdge(node, _targets[jump->target()]);
		map<address_t, Node*>::iterator succ = next(_targets.find(node->_instructions.front()->_addr));
		if (succ != _targets.end() && (!jump || dynamic_cast<CondJump*>(jump)))
			addEdge(node, succ->second);
	}
}


void ControlFlowGraph::addEdge(Node *from, Node *to) {
	from->_out.push_back(to);
	to->_in.push_back(from);
}


void ControlFlowGraph::assignComponents() {
	orderNodes();
	list<Node*> nodes = inPostOrder(_nodes);
	nodes.reverse();
	foreach (Node *u, nodes)
		componentVisit(u, u);
}


void ControlFlowGraph::assignDominators() {
	list<Node*> nodes = inPostOrder(_nodes);
	nodes.reverse();
	nodes.remove(_entry);
	_entry->_dominator = _entry;
	for (bool changed = true; changed; ) {
		changed = false;
		foreach (Node *u, nodes) {
			list<Node*>::iterator it = u->_in.begin();
			while (!(*it)->_dominator)
				it++;
			Node *dom = *it++; // first processed predecessor
			for (; it != u->_in.end(); it++)
				if ((*it)->_dominator)
					dom = dominatorIntersect(*it, dom);
			if (u->_dominator != dom) {
				changed = true;
				u->_dominator = dom;
			}
		}
	}
	_entry->_dominator = 0;
}


// entry node is an interval header
// a node belongs to an interval, if all its immediate predecessors belong the given interval
// otherwise it is an interval header
void ControlFlowGraph::assignIntervals() {
	list<Node*> intervals;
	intervals.push_back(_entry);
	foreach (Node *interval, intervals) {
		interval->_interval = interval;
		for (bool added = true; added; ) {
			added = false;
			foreach (Node *m, _nodes) {
				bool allPredInInterval = true;
				foreach (Node *p, m->_in)
					allPredInInterval &= p->_interval == interval;
				if (!m->_interval && allPredInInterval) {
					added = true;
					m->_interval = interval;
				}
			}
		}
		foreach (Node *m, _nodes) {
			bool anyPredInInterval = false;
			foreach (Node *p, m->_in)
				anyPredInInterval |= p->_interval == interval;
			if (!m->_interval && anyPredInInterval)
				intervals.push_back(m);
		}
	}
}


// a derived graph, given set of intervals, is a graph in which
// all intervals have been collapsed to a single node, and edge
// exists between nodes if there are edges crossing corresponding
// intervals in the original graph
void ControlFlowGraph::extendIntervals() {
	ControlFlowGraph d;
	map<Node*, Node*> trans;
	foreach (Node *interval, intervals()) {
		trans[interval] = d.addNode(interval->_instructions.begin(), interval->_instructions.end());
		trans[interval]->_primitive = interval;
	}
	foreach (Node *interval, intervals())
		foreach (Node *u, interval->_in)
		if (u->_interval != interval)
			d.addEdge(trans[u->_interval], trans[interval]);
	d.setEntry(_entry->_instructions.front()->_addr);
	d.assignIntervals();
	foreach (Node *du, d._nodes)
		foreach (Node *v, _nodes)
		if (v->_interval == du->_primitive)
			v->_interval = du->_interval->_primitive;
}


string graphvizEscapeLabel(const string &s) {
	string ret;
	foreach (char c, s) {
		if (c == '\n' || c == '"' || c == '\\')
			ret.push_back('\\');
		ret.push_back(c == '\n' ? 'l' : c);   // align lines to the left
	}
	return ret;
}

list<Node*> ControlFlowGraph::components() {
	list<Node*> ret;
	assignComponents();
	foreach (Node *u, _nodes)
		if (u->_component == u)
			ret.push_back(u);
	return ret;
}

string ControlFlowGraph::graphvizToString(const string &fontname, int fontsize) {
	stringstream ret;
	ret << "digraph G {" << endl;
	foreach (Node *interval, intervals()) {
		ret << "subgraph " << '"' << "cluster_" << interval << '"' << " {" << endl;
		ret << "style=dotted;" << endl;
		foreach (Node *u, _nodes)
			if (u->_interval == interval) {
				ret << '"' << u << "\"[";
				if (fontname != "")
					ret << "fontname=" << '"' << fontname << "\",";
				if (fontsize != 0)
					ret << "fontsize=" << fontsize << ",";
				ret	<< "shape=box,label=\"<number=" << u->_number;
				if (u->_dominator)
					ret	<< ", dom=" << u->_dominator->_number;
				ret << ">\\n" << graphvizEscapeLabel(u->toString()) << "\"];" << endl;
			}
		ret << "}" << endl;
	}
	foreach (Node *u, _nodes)
		foreach (Node *v, u->_out)
		    ret << '"' << u << "\" -> \"" << v << '"' << ";" << endl;
	ret << "}" << endl;
	return ret.str();
}


list<Node*> ControlFlowGraph::intervals() {
	list<Node*> ret;
	assignIntervals();
	foreach (Node *u, _nodes)
		if (u->_interval == u)
			ret.push_back(u);
	return ret;
}


bool ControlFlowGraph::isReducible() {
	for (size_t size = _nodes.size()+1; size > intervals().size(); size = intervals().size(), extendIntervals())
		;
	return intervals().size() == 1;
}


void ControlFlowGraph::orderNodes() {
	assert(_entry);
	if (!_entry->_number)
		orderVisit(_entry, 0);
}


void ControlFlowGraph::removeJumpsToJumps() {
	for (bool changed = true; changed; ) {
		changed = false;
		foreach (Node *u, _nodes) {
			foreach (Node *v, u->_out) {
				Jump *jump = dynamic_cast<Jump*>(v->_instructions.front());
				if (jump && !dynamic_cast<CondJump*>(jump) && jump->target() != jump->_addr) {
					changed = true;
					replaceEdges(u, v, _targets[jump->target()]);
				}
			}
		}
	}
}


void ControlFlowGraph::removeUnreachableNodes() {
	foreach (Node *u, _nodes)
		if (!u->_number) {
			foreach (Node *v, u->_out)
				v->_in.remove(u);
			foreach (Node *v, u->_in)
				v->_out.remove(u);
		}
	for (list<Node*>::iterator it = _nodes.begin(); it != _nodes.end(); )
		if ((*it)->_number)
			it++;
		else {
			delete *it;
			it = _nodes.erase(it);
		}
}


void ControlFlowGraph::replaceEdges(Node *from, Node *oldTo, Node *newTo) {
	size_t n = count(oldTo->_in.begin(), oldTo->_in.end(), from);
	oldTo->_in.remove(from);
	fill_n(back_inserter(newTo->_in), n, from);
	foreach (Node *&node, from->_out)
		if (node == oldTo)
			node = newTo;
}


void ControlFlowGraph::setEntry(address_t entry) {
	foreach (Node *node, _nodes)
		if (node->_instructions.front()->_addr == entry)
			_entry = node;
}