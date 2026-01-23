#ifndef _NODE_H
#define _NODE_H

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cfloat>
#include <initializer_list>

#include "board.h"
#include "cache.h"

struct Options {
  void UpdateMinProbFromDepth() { min_prob = 1.0 / (1 << (max_depth + 4)); }

  int seed = 1;
  int max_depth = 7;
  int iterations = 1;
  double min_prob = 0.001;
  bool verbose = false;
  bool interactive = false;
  float optimality = 0.9;
  int prefill_rank = 0;
  int max_rank = 0;
  int max_moves = 0;
  bool tuple_moves = true;
  double save_threshold = 0.1;
  bool quiet = false;
  int progress_interval = 100;
};

static Options options;

class Node : public Board {
 public:
  Node() = default;
  Node(int layout[N][N]) : Board(layout) {}

  struct SearchStats {
    long long move_nodes = 0;
    long long tile_nodes = 0;
    long long tile_branches = 0;
    long long eval_calls = 0;
    long long prune_score = 0;
    long long prune_depth = 0;
    long long prune_prob = 0;
    long long cache_lookups = 0;
    long long cache_hits = 0;
    long long cache_updates = 0;
    long long cache_collisions = 0;
    int max_move_depth = 0;
  };

  static void ResetSearchStats() {
    search_stats = SearchStats{};
  }

  static SearchStats GetSearchStats() { return search_stats; }

  void Prefill(int max_rank) {
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) board[x][y] = 1;
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 3; ++x) board[x][y] = max_rank - x * 2 - y;
    board[2][1] = 4;
  }

  static int TileScore(int r) { return r << r; }
  static int Tile(int r) { return r ? 1 << r : 0; }
  int Tile(int x, int y) const { return Tile(board[x][y]); }

  int MaxTile() const { return Tile(MaxRank()); }

  int SumTile() const {
    int sum_tile = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) sum_tile += Tile(x, y);
    return sum_tile;
  }

  int GameScore() const {
    int game_score = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        auto rank = board[x][y];
        if (rank > 1) game_score += (rank - 1) << rank;
      }
    return game_score - num_4_tiles * 4;
  }

  void Line() const {
    for (int x = 0; x < N; ++x) printf("--------");
    puts("-");
  }

  void Show() const {
    Line();
    for (int y = 0; y < N; ++y) {
      putchar('|');
      for (int x = 0; x < N; ++x) {
        auto tile = Tile(x, y);
        if (x == rand_x && y == rand_y)
          printf(" > %d <", tile);
        else if (tile)
          printf("%6d", tile);
        else
          printf("      ");
        putchar(' ');
        putchar('|');
      }
      putchar('\n');
      Line();
    }
    printf("score %d max %d sum %d\n", GameScore(), MaxTile(), SumTile());
  }

  int CountEmptyTiles() const {
    int num_empty_tiles = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x)
        if (board[x][y] == 0) num_empty_tiles++;
    return num_empty_tiles;
  }

  int CountLargeTiles(int large_rank) const {
    int count = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x)
        if (board[x][y] >= large_rank) ++count;
    return count;
  }

  bool GenerateRandomTile() {
    int num_empty_tiles = 0;
    int empty_tiles[N * N];
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x)
        if (board[x][y] == 0) empty_tiles[num_empty_tiles++] = x + y * N;
    if (num_empty_tiles == 0) return false;

    int r = rand() % num_empty_tiles;
    rand_x = empty_tiles[r] % N;
    rand_y = empty_tiles[r] / N;
    board[rand_x][rand_y] = 1 + (rand() % 10 == 0);
    if (board[rand_x][rand_y] == 2) ++num_4_tiles;
    return true;
  }

  void AddTile(int x, int y, bool is_four) {
    assert(board[x][y] == 0);
    rand_x = x;
    rand_y = y;
    board[x][y] = is_four ? 2 : 1;
    if (is_four) ++num_4_tiles;
  }

  int Row(int y) const {
    int v = 0;
    for (int x = 0; x < N; ++x) v = v * 32 + board[x][y];
    return v;
  }

  int Col(int x) const {
    int v = 0;
    for (int y = 0; y < N; ++y) v = v * 32 + board[x][y];
    return v;
  }

  int Evaluate() const {
    if (options.interactive) {
      int score_left = 0, score_right = 0;
      for (int y = 0; y < N; ++y) {
        score_left += score_map[Row(y)].descending;
        score_right += score_map[Row(y)].ascending;
      }
      int score_up = 0, score_down = 0;
      for (int x = 0; x < N; ++x) {
        score_up += score_map[Col(x)].descending;
        score_down += score_map[Col(x)].ascending;
      }
      return std::max(score_left, score_right) + std::max(score_up, score_down);
    } else {
      int score = 0;
      for (int y = 0; y < N; ++y) score += score_map[Row(y)].descending;
      for (int x = 0; x < N; ++x) score += score_map[Col(x)].descending;
      return score;
    }
  }

  int TryAllTiles(int depth, float prob) {
    ++search_stats.tile_nodes;
    ++search_stats.eval_calls;
    int score = Evaluate();
    bool prune_score = score < pass_score;
    bool prune_depth = depth < 0;
    bool prune_prob = prob < options.min_prob;
    if (prune_score || prune_depth || prune_prob) {
      if (prune_score) ++search_stats.prune_score;
      if (prune_depth) ++search_stats.prune_depth;
      if (prune_prob) ++search_stats.prune_prob;
      return score;
    }

    unsigned long long compact_board = 0;
    int empty_tiles = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        compact_board = compact_board * 16 + board[x][y];
        empty_tiles += board[x][y] == 0;
      }

    if (!skip_cache && cache.Lookup(compact_board, prob, depth, &score))
      return score;

    float tile2_prob = prob / empty_tiles * 0.9;
    float tile4_prob = prob / empty_tiles * 0.1;
    int total_score = 0;
    for (int y = 0; y < N; ++y) {
      for (int x = 0; x < N; ++x) {
        if (board[x][y]) continue;

        board[x][y] = 1;
        ++search_stats.tile_branches;
        total_score += 0.9 * TryAllMoves(depth, tile2_prob);
        board[x][y] = 2;
        ++search_stats.tile_branches;
        total_score += 0.1 * TryAllMoves(depth, tile4_prob);
        board[x][y] = 0;
      }
    }
    cache.Update(compact_board, prob, depth, total_score / empty_tiles);
    return total_score / empty_tiles;
  }

  int TryAllMoves(int depth, float prob, int* best_move = nullptr,
                  int move_scores[4] = nullptr) {
    ++search_stats.move_nodes;
    int ply = search_root_depth - depth;
    if (ply > search_stats.max_move_depth) search_stats.max_move_depth = ply;
    int best_score = game_over_score;
    if (move_scores) {
      for (int i = 0; i < 4; ++i) move_scores[i] = game_over_score;
    }
    if (best_move) *best_move = -1;
    for (int i = 0; i < 4; i++) {
      Node m = *this;
      if (!(m.*moves[i])()) continue;

      int score = m.TryAllTiles(depth - 1, prob);
      if (move_scores) move_scores[i] = score;
      if (best_score == game_over_score || best_score < score) {
        best_score = score;
        if (best_move) *best_move = i;
      }
    }
    return best_score;
  }

  int Search(int depth, int* best_move, int move_scores[4] = nullptr) {
    search_root_depth = depth;
    int max_tile_score = TileScore(MaxRank());
    int score = Evaluate();
    if (options.tuple_moves) {
#ifdef BIG_TUPLES
      pass_score = score < max_tile_score * 2 ? -INT_MAX : max_tile_score * 2;
#else
      pass_score = score < 0 ? -INT_MAX : max_tile_score * 2;
#endif
      game_over_score = -std::max(1 << 17, max_tile_score * 2);
    } else {
      pass_score = -INT_MAX;
      game_over_score = -1 << 22;
    }
    skip_cache = false;
    auto h_score = TryAllMoves(depth, 1, best_move, move_scores);
#ifdef BIG_TUPLES
    if (h_score < 0) {
        pass_score = -INT_MAX;
        game_over_score = -1 << 22;
        skip_cache = true;
        h_score = TryAllMoves(depth, 1, best_move, move_scores);
    }
#endif
    return h_score;
  }

  static void BuildScoreMap();
  static Cache cache;
  static SearchStats search_stats;

 private:
  struct Scores {
    int ascending, descending;
  };
  static Scores score_map[1 << 20];

  int num_4_tiles = 0;
  int rand_x = -1;
  int rand_y = -1;
  int pass_score;
  int game_over_score;
  bool skip_cache;
  static int search_root_depth;
};

// static
Node::Scores Node::score_map[1 << 20];
Cache Node::cache;
Node::SearchStats Node::search_stats;
int Node::search_root_depth = 0;

// static
void Node::BuildScoreMap() {
  int line[N];
  for (int i = 0; i < 1 << 20; ++i) {
    for (int j = 0; j < N; ++j) line[j] = (i >> (j * 5)) & 0x1f;
    int score = TileScore(line[0]);
    for (int x = 0; x < N - 1; ++x) {
      int a = TileScore(line[x]), b = TileScore(line[x + 1]);
      score += a >= b ? a + b : (a - b) * 12;
      score += a == b ? a : 0;
    }

    int k = 0;
    for (int j = 0; j < N; ++j) k = k * 32 + line[j];
    score_map[k].descending = score_map[i].ascending = score;
  }
}

#endif
