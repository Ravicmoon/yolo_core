#include "BipartiteGraph.h"

#include <exception>
#include <iostream>

Edge BipartiteGraph::GetMaxWeightEdge(Vertex& agent)
{
  size_t row_num = agent.GetVID();
  size_t col_num = 0;
  double max_wt = bg_matrix[row_num][0].GetWeight();
  // for(vector<Edge>::iterator itr = bg_matrix[row_num].begin(); itr !=
  // bg_matrix[row_num].begin() + bg_matrix.ncols(); itr++)
  for (unsigned int i = 0; i < bg_matrix[0].size(); i++)
    if (bg_matrix[row_num][i].GetWeight() > max_wt)
    {
      max_wt = bg_matrix[row_num][i].GetWeight();
      col_num = i;
    }
  // cout<<row_num<<" "<<col_num<<" "<<max_wt<<endl;
  return bg_matrix[row_num][col_num];
}

void BipartiteGraph::ConstructBG(Matrix& m)
{
  num_agents = m.size();
  num_tasks = m[0].size();
  bg_matrix = m;

  agents.resize(num_agents);
  tasks.resize(num_tasks);
  // bg_matrix.resize(num_agents, num_tasks);
  bg_matrix.resize(num_agents);
  for (unsigned int i = 0; i < num_agents; i++) bg_matrix[i].resize(num_tasks);

  // assign task VID and assign label to be 0;
  int _vid = 0;
  for (auto itr = tasks.begin(); itr != tasks.end(); itr++)
  {
    itr->SetVID(_vid++);
    itr->SetObj("TASK");
    itr->SetLabel(0);
    itr->path.clear();
  }

  // assign agent VID and assign label to be max;
  _vid = 0;
  for (auto itr = agents.begin(); itr != agents.end(); itr++)
  {
    itr->SetVID(_vid++);
    itr->SetObj("AGENT");
    itr->SetLabel(this->GetMaxWeightEdge(*itr).GetWeight());
    itr->path.clear();
  }

  // init all edges
  for (unsigned int i = 0; i < num_agents; i++)
    for (unsigned int j = 0; j < num_tasks; j++)
      bg_matrix[i][j].SetEID(EID(i, j));
}

void BipartiteGraph::DisplayLabels(std::vector<Vertex>& v)
{
  for (auto itr = v.begin(); itr != v.end(); itr++)
    std::cout << itr->GetLabel() << " ";
  std::cout << std::endl;
}

void BipartiteGraph::DisplayLabels(void)
{
  std::cout << "Labels for agents:" << std::endl;
  this->DisplayLabels(this->agents);
  std::cout << "Labels for tasks:" << std::endl;
  this->DisplayLabels(this->tasks);
}

bool BipartiteGraph::CheckFeasibility(
    Matrix& _m, std::vector<Vertex>& _agents, std::vector<Vertex>& _tasks)
{
  // check the size for args
  if (_m.size() != _agents.size() || _m[0].size() != _tasks.size())
  {
    throw std::runtime_error(
        "[BipartiteGraph::CheckFeasibility] size discrepency found during "
        "feasibility checking");
  }

  bool result = true;

  for (size_t i = 0; i < _m.size(); i++)
  {
    for (size_t j = 0; j < _m[0].size(); j++)
    {
      if (_m[i][j].GetWeight() -
              (_agents[i].GetLabel() + _tasks[j].GetLabel()) >
          DOUBLE_EPSILON)
      {
        if (_m[i][j].GetWeight() > _agents[i].GetLabel() + _tasks[j].GetLabel())
        {
          throw std::runtime_error(
              "[BipartiteGraph::CheckFeasibility] abels are not feasible any "
              "more!");
          std::cout << "\tGetweight: " << _m[i][j].GetWeight() << std::endl;
          std::cout << "\tGetAlabel: " << _agents[i].GetLabel() << std::endl;
          std::cout << "\tGetTlabel: " << _tasks[j].GetLabel() << std::endl;
        }
        result = false;
      }
    }
  }
  return result;
}

bool BipartiteGraph::CheckFeasibility(void)
{
  bool res = this->CheckFeasibility(bg_matrix, agents, tasks);
  if (!res)
    throw std::runtime_error(
        "[BipartiteGraph::CheckFeasibility] bipartite graph is not feasible!");

  return res;
}
