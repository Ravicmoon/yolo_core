
/******************************************************************************
*                                                                             *
*  C++ Implementation of Hungarian Algorithm (Bipartite graph version).       *
*  Lantao Liu <lantao@cs.tamu.edu>                                            *
*  Nov 1, 2009                                                                *
*                                                                             *
******************************************************************************/

///////////////////////////////////////////////////////////////////////////////
// File name: Hungarian.h
// This file defines the main algorithm of Kuhn-Munkres Hungarian Method.
// The algorithm is decomposed into functional modules, and each module is
// encapsulated as a function here.
// Lantao Liu, Nov 1, 2009
///////////////////////////////////////////////////////////////////////////////
#pragma once
#include "BipartiteGraph.h"

///////////////////////////////////////////////////////////////////////////////
//
// Hungarian class: defines basic modules (methods) for Hungarian procedure.
//
///////////////////////////////////////////////////////////////////////////////

class Hungarian
{
public:
	Hungarian(){ S.reserve(AGENTS_SIZE); T.reserve(TASKS_SIZE); }
	Hungarian(BipartiteGraph& _bg): bg(_bg)
	{
		S.reserve(_bg.GetNumAgents()); 
		T.reserve(_bg.GetNumTasks());
		N.reserve(_bg.GetNumTasks());
	}
	~Hungarian(){}

	//To access private bipartite graph member
	BipartiteGraph* GetBG(void){ return &bg; }

	//Judge if the bipartite graph is perfect or not, to control program to halt
	bool IsPerfect(BipartiteGraph& _bg)
	{
		bool result = _bg.GetNumMatched() == _bg.GetNumAgents() ? true: false;
		return result;
	}
	bool IsPerfect(void){ return this->IsPerfect(this->bg); } 

	//Start a new alternating tree, pass the root to set S
	//and initialize all other sets which need to initialized.
	void InitNewRoot(BipartiteGraph& _bg, VID root, std::vector<VID>& _S, std::vector<VID>& _T);

	//Randomly pick an unmatched (free) agent, as new root
	VID PickFreeAgent(BipartiteGraph& _bg);

	//Randomly pick a new task in set {N-T} during alternating tree
	VID PickAvailableTask(std::vector<VID>& _T, std::vector<VID>& _N);

	//Update labels, forcing N != T, return minimal delta
	double UpdateLabels(BipartiteGraph& _bg);  

	//Refresh the whole bipartite graph, update all available sets
	//based on new state of bipartite graph _bg
	void RefreshBG(BipartiteGraph& _bg, const std::vector<VID>& _S, const std::vector<VID>& _T, std::vector<VID>& _N, std::vector<EID>& _EG, std::vector<EID>& _M);

	//Judge if need relabel or not, via comparing sets T and N
	bool NeedReLabel(std::vector<VID>& _T,  std::vector<VID>& _N);

	//Find neighbors and return a neighbor-set, computed from EG and S
	std::vector<VID> FindNeighbors(const std::vector<EID>& _EG, const std::vector<VID>& _S);

	//Breadth-first Searching augmenting path, return the path as a vector of EID
	//x: root
	//y: unmatched task
	std::vector<EID> BFSAugmentingPath(BipartiteGraph& _bg, VID x, VID y);

	//Augment the path, via the information of augmenting path
	//flip the matched edges and unmatched edges, M size increases 1
	//_path: an augmenting path
	void AugmentPath(BipartiteGraph& _bg, std::vector<EID>& _path);

	//When found a matched task, put it and its mate (matching) agent
	//into T and S correspondingly, and color them to flag as having been 
	//visited and covered
	void ExtendTree(BipartiteGraph& _bg, VID x, std::vector<VID>& _S, std::vector<VID>& _T);

	void HungarianAlgo(BipartiteGraph& _bg, std::vector<VID>& _S, std::vector<VID>& _T,
		std::vector<VID>& _N, std::vector<EID>& _EG, std::vector<EID>& _M);
	void HungarianAlgo(void);

	//Calculate optimal value: the sum of weights of matched edges
	double OptimalValue(BipartiteGraph& _bg, std::vector<EID>& _M);

	//Display data
	void DisplayData(std::vector<EID>&);
	void DisplayData(std::vector<VID>&);
	//The five args are in order of S, T, N, EG, M. Set true if want to display
	void DisplayData(bool s=false, bool t=false, bool n=false, bool eg=false, bool m=false);

	void DisplayLabels(std::vector<Vertex>&);

	void DisplayMatrix(Matrix&);
	void DisplayConfig(BipartiteGraph&);

//data member
private:
	const static bool m_bVerbose = false;

	//A basic bipartite graph is needed
	BipartiteGraph bg;

	//All sets required in Hungarian algo
	std::vector<VID> S;
	std::vector<VID> T;
	std::vector<VID> N;
	std::vector<EID> EG;
	std::vector<EID> M; 
};