#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <functional>
#include <iomanip>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include "node.h"
#include "plan.h"
#include "tuple.h"

struct Snapshot {
  Node node;
  int move;
  float prob;
};

double Elapse(const timeval& from, const timeval& to) {
  return (to.tv_sec - from.tv_sec) + (double(to.tv_usec) - from.tv_usec) * 1e-6;
}

char GetKey() {
  char buf = 0;
  struct termios old = {0};
  if (tcgetattr(0, &old) < 0) perror("tcsetattr()");
  old.c_lflag &= ~ICANON;
  old.c_lflag &= ~ECHO;
  old.c_cc[VMIN] = 1;
  old.c_cc[VTIME] = 0;
  if (tcsetattr(0, TCSANOW, &old) < 0) perror("tcsetattr ICANON");
  if (read(0, &buf, 1) < 0) perror("read()");
  old.c_lflag |= ICANON;
  old.c_lflag |= ECHO;
  if (tcsetattr(0, TCSADRAIN, &old) < 0) perror("tcsetattr ~ICANON");
  return (buf);
}

int InteractivePlay(Node* n, int* move, float prob) {
  auto show_move = [](int move, float prob) {
    if (move >= 0)
      printf("%5s %.3f", Board::move_names[move], prob);
    else
      printf("deadend");
    printf(
        ", Space=accept, A|H=left, S|J=down, W|K=up, D|L=right, U=undo, "
        "Q=quit\n");
  };

  static std::vector<Snapshot> history;
  history.push_back({*n, *move, prob});
  show_move(*move, prob);

  Node next = *n;
  int num_undos = 0;
  bool moved = false, quit = false;
  do {
    switch (toupper(GetKey())) {
      case ' ':
        *move = history.back().move;
        moved = true;
        break;
      case 'W':
      case 'K':
        *move = 0;
        moved = (next.*Node::moves[0])();
        break;
      case 'A':
      case 'H':
        *move = 1;
        moved = (next.*Node::moves[1])();
        break;
      case 'D':
      case 'L':
        *move = 2;
        moved = (next.*Node::moves[2])();
        break;
      case 'S':
      case 'J':
        *move = 3;
        moved = (next.*Node::moves[3])();
        break;
      case 'U':
        if (history.size() > 1) {
          ++num_undos;
          history.pop_back();
          *n = history.back().node;
          *move = history.back().move;
          prob = history.back().prob;
          n->Show();
          show_move(*move, prob);
          next = *n;
        }
        break;
      case 'Q':
        quit = true;
        *move = -1;
        break;
    }
  } while (!moved && !quit);
  return num_undos;
}

std::unique_ptr<Tuple10> tuple10;
std::unique_ptr<Tuple11> tuple11;
std::unique_ptr<BlockPlan> block_plan;
std::unique_ptr<LinePlan> line_plan;

struct Valuation {
  float prob(int max_rank) const {
    if (type == kSearch) return value / 1000;
    else return value;
  }

  void Show(int max_rank) const {
    if (type == kSearch)
      printf("from search, h-score %.3f\n", prob(max_rank));
    else if (type == kTuple10)
      printf("from lookup-10, prob %.3f\n", prob(max_rank));
    else if (type == kTuple11)
      printf("from lookup-11, prob %.3f\n", prob(max_rank));
    else if (type == kBlockPlan)
      printf("from block plan, prob %.3f\n", prob(max_rank));
    else
      printf("from line plan, prob %.3f\n", prob(max_rank));
  }

  enum Type {kInvalid, kTuple11, kTuple10, kLinePlan, kBlockPlan, kSearch};
  Type type;
  float value;
};

const char* ValuationTypeName(Valuation::Type type) {
  switch (type) {
    case Valuation::kTuple11:
      return "tuple11";
    case Valuation::kTuple10:
      return "tuple10";
    case Valuation::kLinePlan:
      return "line_plan";
    case Valuation::kBlockPlan:
      return "block_plan";
    case Valuation::kSearch:
      return "search";
    default:
      return "invalid";
  }
}

std::string FormatFloat(double value, int precision = 6) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed, std::ios::floatfield);
  oss << std::setprecision(precision) << value;
  return oss.str();
}

class StepLogger {
 public:
  StepLogger() = default;
  ~StepLogger() { Close(); }

  bool Open(const std::string& path, bool gzip) {
    Close();
    gzip_ = gzip;
    path_ = path;
    buffer_.clear();
    buffer_.shrink_to_fit();
    if (gzip_) {
      file_gz_ = gzopen(path.c_str(), "wb");
      if (!file_gz_) return false;
    } else {
      file_stream_.open(path.c_str(),
                        std::ios::out | std::ios::trunc | std::ios::binary);
      if (!file_stream_.is_open()) return false;
    }
    is_open_ = true;
    return true;
  }

  bool WriteLine(const std::string& line) {
    if (!is_open_) return false;
    try {
      buffer_.append(line);
      buffer_.push_back('\n');
    } catch (const std::bad_alloc&) {
      return false;
    }
    return true;
  }

  bool Close() {
    if (!is_open_) {
      buffer_.clear();
      buffer_.shrink_to_fit();
      path_.clear();
      return true;
    }
    bool ok = true;
    if (gzip_) {
      if (file_gz_) {
        if (!buffer_.empty()) {
          const auto len = static_cast<int>(buffer_.size());
          if (gzwrite(file_gz_, buffer_.data(), len) != len) ok = false;
        }
        if (gzclose(file_gz_) != Z_OK) ok = false;
        file_gz_ = nullptr;
      }
    } else {
      if (file_stream_.is_open()) {
        if (!buffer_.empty()) {
          file_stream_.write(buffer_.data(), buffer_.size());
          if (!file_stream_) ok = false;
        }
        file_stream_.close();
        if (!file_stream_) ok = false;
      }
    }
    is_open_ = false;
    buffer_.clear();
    std::string().swap(buffer_);
    path_.clear();
    return ok;
  }

  const std::string& Path() const { return path_; }

 private:
  bool gzip_ = false;
  bool is_open_ = false;
  gzFile file_gz_ = nullptr;
  std::ofstream file_stream_;
  std::string buffer_;
  std::string path_;
};

struct LastSearchMetrics {
  bool used = false;
  Node::SearchStats stats;
};

static LastSearchMetrics last_search_metrics;

Valuation SuggestMove(Node& n, int* m, bool lookup_only = false,
                      int move_scores[4] = nullptr) {
  last_search_metrics.used = false;
  if (tuple11) {
    auto prob = tuple11->SuggestMove(n, m);
#ifdef BIG_TUPLES
    if (prob > 0.1) return {Valuation::kTuple11, prob};
#else
    if (prob >= 0.9) return {Valuation::kTuple11, prob};
#endif
  }
  if (tuple10) {
    auto prob = tuple10->SuggestMove(n, m);
    if (prob > 0) return {Valuation::kTuple10, prob};
  }
  if (line_plan) {
    auto suggestion = line_plan->SuggestMove(n);
    *m = suggestion.move;
    auto prob = suggestion.prob;
    if (prob > 0) return {Valuation::kLinePlan, prob};
  }
  if (block_plan) {
    auto suggestion = block_plan->SuggestMove(n);
    *m = suggestion.move;
    auto prob = suggestion.prob;
    if (prob > 0) return {Valuation::kBlockPlan, prob};
  }
  if (lookup_only) {
    last_search_metrics.used = false;
    return {Valuation::kSearch, 0};
  }
  last_search_metrics.used = true;
  Node::ResetSearchStats();
  long long lookups_before = Node::cache.LookupCount();
  long long hits_before = Node::cache.HitCount();
  long long updates_before = Node::cache.UpdateCount();
  long long collisions_before = Node::cache.CollisionCount();
  auto value = (float)n.Search(options.max_depth, m, move_scores);
  last_search_metrics.stats = Node::GetSearchStats();
  last_search_metrics.stats.cache_lookups =
      Node::cache.LookupCount() - lookups_before;
  last_search_metrics.stats.cache_hits = Node::cache.HitCount() - hits_before;
  last_search_metrics.stats.cache_updates =
      Node::cache.UpdateCount() - updates_before;
  last_search_metrics.stats.cache_collisions =
      Node::cache.CollisionCount() - collisions_before;
  return {Valuation::kSearch,
          value};
}

float LookupTuple(Node &n, Valuation::Type type) {
  int ignore;
  if (type == Valuation::kTuple11)
    return tuple11->SuggestMove(n, &ignore);
  else
    return tuple10->SuggestMove(n, &ignore);
}

float RollOutWithTuple(Node &n, Valuation::Type type) {
  int empty_tiles = n.CountEmptyTiles();
  float tile2_prob = 1.0 / empty_tiles * 0.9;
  float tile4_prob = 1.0 / empty_tiles * 0.1;
  float total_prob = 0;
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      if (n[x][y]) continue;

      n[x][y] = 1;
      total_prob += LookupTuple(n, type) * tile2_prob;
      n[x][y] = 2;
      total_prob += LookupTuple(n, type) * tile4_prob;
      n[x][y] = 0;
    }
  }
  return total_prob;
}

void AnalyzeMove(Node& n, int move, bool lookup_only = false) {
  int best_move;
  auto best_valuation = SuggestMove(n, &best_move, lookup_only);
  if (move == best_move) return;

  Node n1 = n;
  if (!(n1.*Node::moves[move])()) return;

  Valuation valuation = {best_valuation.type, 0};
  if (best_valuation.type == Valuation::kTuple11 ||
      best_valuation.type == Valuation::kTuple10) {
    valuation.value = RollOutWithTuple(n1, best_valuation.type);
  } else {
    if (lookup_only) return;
    valuation.value = n1.TryAllTiles(options.max_depth - 1, 1);
  }
  bool suboptimal = (best_valuation.value >= 0)
    ? (valuation.value < options.optimality * best_valuation.value)
    : (valuation.value < 1 / options.optimality * best_valuation.value);
  if (suboptimal &&
      (!lookup_only ||
       (valuation.type != Valuation::kSearch && valuation.value > 0))) {
    n.Show();
    printf("***** %s %.3f < %s %.3f *****\n",
           Board::move_names[move], valuation.prob(n.MaxRank()),
           Board::move_names[best_move], best_valuation.prob(n.MaxRank()));
  }
}

void AnalyzeLog(char* log_file) {
  options.interactive = true;

  // The table for converting the code defined by https://2048league.ml/replay/.
  static const char* ascii[128] = {
      " ", "!", "\"", "#", "$",  "%", "&", "'", "(", ")", "*", "+", ",", "-",
      ".", "/", "0",  "1", "2",  "3", "4", "5", "6", "7", "8", "9", ":", ";",
      "<", "=", ">",  "?", "@",  "A", "B", "C", "D", "E", "F", "G", "H", "I",
      "J", "K", "L",  "M", "N",  "O", "P", "Q", "R", "S", "T", "U", "V", "W",
      "X", "Y", "Z",  "[", "\\", "]", "^", "_", "`", "a", "b", "c", "d", "e",
      "f", "g", "h",  "i", "j",  "k", "l", "m", "n", "o", "p", "q", "r", "s",
      "t", "u", "v",  "w", "x",  "y", "z", "{", "|", "}", "~", "Ç", "ü", "é",
      "â", "ä", "à",  "å", "ç",  "ê", "ë", "è", "ï", "î", "ì", "Ä", "Å", "É",
      "æ", "Æ", "ô",  "ö", "ò",  "û", "ù", "ÿ", "Ö", "Ü", "ø", "£", "Ø", "×",
      "ƒ", "á"};

  std::fstream file(log_file);
  std::string log((std::istreambuf_iterator<char>(file)),
                  std::istreambuf_iterator<char>());
  if (log.empty()) {
    printf("Log file %s doesn't exist.\n", log_file);
    exit(-1);
  }

  Node n;
  int num_tiles = 0;
  for (size_t i = 0; i < log.size(); ++num_tiles) {
    size_t code = 0;
    for (; code < sizeof(ascii) / sizeof(ascii[0]); ++code) {
      if (strncmp(log.c_str() + i, ascii[code], strlen(ascii[code])) == 0) {
        i += strlen(ascii[code]);
        break;
      }
    }
    if (num_tiles >= 2) {
      int m = "0231"[code / 32 % 4] - '0';
      AnalyzeMove(n, m, /*lookup_only*/true);
      bool moved = (n.*Node::moves[m])();
      if (!moved) break;
    }
    auto x = code % 16 / 4, y = code % 16 % 4;
    bool is_four = code / 16 % 2;
    n.AddTile(x, y, is_four);
  }
}

int CharToRank(char ch) {
  if ('0' <= ch && ch <= '9') return ch - '0';
  if ('A' <= ch && ch <= 'G') return ch - 'A' + 10;
  if ('a' <= ch && ch <= 'g') return ch - 'a' + 10;
  return 0;
}

void RunAgent(int client_socket) {
  char buffer[1 << 16] = {0};
  while (true) {
    buffer[0] = '\0';
    if (read(client_socket, buffer, sizeof(buffer)) < 0) break;
    buffer[sizeof(buffer) - 1] = '\0';

    char* get = strstr(buffer, "GET /move?board=");
    char* post = strstr(buffer, "POST /analyze?board=");
    if (!get && !post) break;
    char* board = get ? strchr(get, '=') + 1 : strchr(post, '=') + 1;
    if (strlen(board) < N * N) break;

    int layout[N][N] = {0};
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) layout[y][x] = CharToRank(*board++);

    Node n(layout);
    if (post) {
      char* direction = strchr(board, '=') + 1;
      int m = -1;
      switch (*direction) {
        case 'u': m = 0; break;
        case 'l': m = 1; break;
        case 'r': m = 2; break;
        case 'd': m = 3; break;
      }
      if (m == -1) break;

      AnalyzeMove(n, m);
      std::string reply("HTTP/1.1 200 OK\n"
                        "Access-Control-Allow-Origin: *\n"
                        "Content-Type: text/plain\n"
                        "Content-Length: 0\n"
                        "\n");

      if (write(client_socket, reply.c_str(), reply.size()) < 0) break;
    } else {
      int m;
      auto valuation = SuggestMove(n, &m);
      if (options.verbose) {
        n.Show();
        if (m >= 0) {
          printf("%5s ", Board::move_names[m]);
          valuation.Show(n.MaxRank());
        }
      }

      std::string reply("HTTP/1.1 200 OK\n"
                        "Access-Control-Allow-Origin: *\n"
                        "Content-Type: text/plain\n"
                        "Content-Length: 1\n"
                        "\n");
      reply += m >= 0 ? Board::move_names[m][0] : 'g';

      if (write(client_socket, reply.c_str(), reply.size()) < 0) break;
    }
  }
  close(client_socket);
}

void RunServer(int server_port) {
  // Creating socket file descriptor
  int server_socket;
  if ((server_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("In socket");
    exit(EXIT_FAILURE);
  }
  int enable = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &enable,
                 sizeof(enable)) < 0) {
    perror("In setsockopt");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(server_port);

  memset(address.sin_zero, '\0', sizeof address.sin_zero);

  if (bind(server_socket, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("In bind");
    exit(EXIT_FAILURE);
  }
  if (listen(server_socket, 10) < 0) {
    perror("In listen");
    exit(EXIT_FAILURE);
  }

  puts("Server ready");
  while (true) {
    const int addrlen = sizeof(address);
    int client_socket;
    if ((client_socket = accept(server_socket, (struct sockaddr*)&address,
                                (socklen_t*)&addrlen)) < 0) {
      perror("In accept");
      exit(EXIT_FAILURE);
    }
    std::thread t(std::bind(RunAgent, client_socket));
    t.detach();
  }

  close(server_socket);
}

int main(int argc, char* argv[]) {
  options.seed = time(nullptr);
  options.UpdateMinProbFromDepth();
  char* log_file = nullptr;
  const char* json_log_path = nullptr;
  bool gzip_json_logging = false;
  int server_port = 0;
  bool print_search_metrics = false;
  int c;
  while ((c = getopt(argc, argv, "d:i:m:p:s:vIL:O:P:R:S:TJ:qF:ZM")) != -1) {
    switch (c) {
      case 'd':
        options.max_depth = atoi(optarg);
        options.UpdateMinProbFromDepth();
        break;
      case 'i':
        options.iterations = atoi(optarg);
        break;
      case 'm':
        options.max_moves = atoi(optarg);
        break;
      case 'p':
        options.min_prob = atof(optarg);
        break;
      case 's':
        options.save_threshold = atof(optarg);
        break;
      case 'v':
        options.verbose = true;
        break;
      case 'I':
        options.interactive = true;
        break;
      case 'L':
        log_file = optarg;
        break;
      case 'O':
        options.optimality = atof(optarg);
        break;
      case 'P':
        options.prefill_rank = atoi(optarg);
        break;
      case 'R':
        options.max_rank = atoi(optarg);
        break;
      case 'S':
        server_port = atoi(optarg);
        break;
      case 'T':
        options.tuple_moves = false;
        break;
      case 'J':
        json_log_path = optarg;
        break;
      case 'q':
        options.quiet = true;
        break;
      case 'F':
        options.progress_interval = atoi(optarg);
        break;
      case 'Z':
        gzip_json_logging = true;
        break;
      case 'M':
        print_search_metrics = true;
        break;
    }
  }
  if (optind < argc) options.seed = atoi(argv[optind]);
  srand(options.seed);
  if (options.max_moves < 0) {
    fprintf(stderr, "max moves must be non-negative\n");
    return 1;
  }

  std::string json_base_path;
  bool enable_json_logging = false;
  if (json_log_path) {
    json_base_path = json_log_path;
    enable_json_logging = true;
  }

  Node::BuildMoveMap();
  Node::BuildScoreMap();
  if (options.tuple_moves) {
    tuple10.reset(new Tuple10(options.save_threshold));
    tuple11.reset(new Tuple11(options.save_threshold));
  } else {
    block_plan.reset(new BlockPlan);
    // line_plan.reset(new LinePlan);
  }

  if (log_file) {
    AnalyzeLog(log_file);
    return 0;
  }
  if (server_port) {
    RunServer(server_port);
    return 0;
  }

  long sum_game_scores = 0;
  int max_rank_freq[N * N + 1];
  memset(max_rank_freq, 0, sizeof(max_rank_freq));
  long long total_moves_logged = 0;

  for (int i = 0; i < options.iterations; ++i) {
    srand(options.seed + i);
    Node::cache.Clear();
    Node::cache.ResetStats();
    Node::ResetSearchStats();
#if 1
    Node n;
    if (options.prefill_rank) {
      n.Prefill(options.prefill_rank);
    } else {
      n.GenerateRandomTile();
      n.GenerateRandomTile();
    }
#else
    int layout[N][N] = {{14, 13, 12, 11},  // row 0
                        {1, 0, 0, 0},      // row 1
                        {0, 0, 0, 0},      // row 2
                        {0, 0, 0, 0}};     // row 3
    Node n(layout);
#endif

    timeval start, finish;
    gettimeofday(&start, NULL);

    StepLogger step_logger;
    std::string game_file_prefix;
    std::string steps_file_path;
    bool step_log_ready = false;
    bool step_log_success = false;
    if (enable_json_logging) {
      std::ostringstream path_builder;
      path_builder << json_base_path << "_game" << std::setfill('0')
                   << std::setw(6) << i;
      game_file_prefix = path_builder.str();
      steps_file_path =
          game_file_prefix + (gzip_json_logging ? ".jsonl.gz" : ".jsonl");
      if (!step_logger.Open(steps_file_path, gzip_json_logging)) {
        fprintf(stderr, "Failed to open JSONL step log %s\n",
                steps_file_path.c_str());
      } else {
        step_log_ready = true;
        step_log_success = true;
      }
    }

    int num_moves = 0;
    int prev_max_rank = 0;
    do {
      if (options.max_moves > 0 && num_moves >= options.max_moves) break;
      if (!options.quiet && (options.verbose || options.interactive)) n.Show();

      int max_rank = n.MaxRank();
      if (max_rank == options.max_rank) break;

      int move_scores[4] = {0, 0, 0, 0};
      int m;
      auto valuation = SuggestMove(n, &m, /*lookup_only=*/false,
                                   step_log_ready ? move_scores : nullptr);
      auto prob = valuation.prob(max_rank);

      if (step_log_ready && m >= 0) {
        std::array<double, 4> branch_values = {0.0, 0.0, 0.0, 0.0};
        std::array<bool, 4> branch_valid = {false, false, false, false};
        if (valuation.type == Valuation::kTuple11 ||
            valuation.type == Valuation::kTuple10) {
          for (int move_idx = 0; move_idx < 4; ++move_idx) {
            Node candidate = n;
            if (!(candidate.*Node::moves[move_idx])()) continue;
            branch_valid[move_idx] = true;
            branch_values[move_idx] =
                RollOutWithTuple(candidate, valuation.type);
          }
        } else {
          for (int move_idx = 0; move_idx < 4; ++move_idx) {
            Node candidate = n;
            if (!(candidate.*Node::moves[move_idx])()) continue;
            branch_valid[move_idx] = true;
            branch_values[move_idx] = move_scores[move_idx] / 1000.0;
          }
        }

        const int step_index = num_moves;
        std::ostringstream line;
        line << '{';
        line << "\"seed\":" << options.seed + i << ',';
        line << "\"step_index\":" << step_index << ',';
        line << "\"max_rank\":" << max_rank << ',';
        line << "\"move\":\"" << Board::move_names[m] << "\",";
        line << "\"valuation_type\":\"" << ValuationTypeName(valuation.type)
             << "\",";
        line << "\"valuation\":" << FormatFloat(prob) << ',';
        line << "\"board\":[";
        bool first = true;
        for (int y = 0; y < N; ++y) {
          for (int x = 0; x < N; ++x) {
            if (!first) line << ',';
            line << n[x][y];
            first = false;
          }
        }
        line << "],";
        line << "\"branch_evs\":{";
        for (int move_idx = 0; move_idx < 4; ++move_idx) {
          if (move_idx) line << ',';
          line << "\"" << Board::move_names[move_idx] << "\":";
          if (branch_valid[move_idx])
            line << FormatFloat(branch_values[move_idx]);
          else
            line << "null";
        }
        line << "},";
        line << "\"search\":{";
        line << "\"used\":" << (last_search_metrics.used ? "true" : "false")
             << ",";
        if (last_search_metrics.used) {
          const auto& s = last_search_metrics.stats;
          long long total_nodes = s.move_nodes + s.tile_nodes;
          double hit_rate =
              s.cache_lookups ? (double)s.cache_hits * 100.0 / s.cache_lookups
                              : 0.0;
          line << "\"total_nodes\":" << total_nodes << ",";
          line << "\"move_nodes\":" << s.move_nodes << ",";
          line << "\"tile_nodes\":" << s.tile_nodes << ",";
          line << "\"tile_branches\":" << s.tile_branches << ",";
          line << "\"eval_calls\":" << s.eval_calls << ",";
          line << "\"prune_score\":" << s.prune_score << ",";
          line << "\"prune_depth\":" << s.prune_depth << ",";
          line << "\"prune_prob\":" << s.prune_prob << ",";
          line << "\"cache_lookups\":" << s.cache_lookups << ",";
          line << "\"cache_hits\":" << s.cache_hits << ",";
          line << "\"cache_updates\":" << s.cache_updates << ",";
          line << "\"cache_collisions\":" << s.cache_collisions << ",";
          line << "\"cache_hit_rate\":" << FormatFloat(hit_rate) << ",";
          line << "\"max_move_depth\":" << s.max_move_depth;
        } else {
          line << "\"total_nodes\":null,";
          line << "\"move_nodes\":null,";
          line << "\"tile_nodes\":null,";
          line << "\"tile_branches\":null,";
          line << "\"eval_calls\":null,";
          line << "\"prune_score\":null,";
          line << "\"prune_depth\":null,";
          line << "\"prune_prob\":null,";
          line << "\"cache_lookups\":null,";
          line << "\"cache_hits\":null,";
          line << "\"cache_updates\":null,";
          line << "\"cache_collisions\":null,";
          line << "\"cache_hit_rate\":null,";
          line << "\"max_move_depth\":null";
        }
        line << "}";
        line << '}';
        if (!step_logger.WriteLine(line.str())) {
          fprintf(stderr, "Failed to write JSONL step log %s\n",
                  steps_file_path.c_str());
          step_log_ready = false;
          step_log_success = false;
        }
      }

      if (options.interactive) num_moves -= InteractivePlay(&n, &m, prob);

      if (m < 0) break;

      bool moved = (n.*Node::moves[m])();
      assert(moved);
      ++num_moves;

      if (options.interactive && !options.quiet) {
        printf("#%d: %s\n", num_moves, Board::move_names[m]);
      } else if (options.verbose && !options.quiet) {
        printf("#%d: %5s ", num_moves, Board::move_names[m]);
        valuation.Show(max_rank);
      } else if (!options.quiet && max_rank != prev_max_rank) {
        printf("\r%7d", Node::Tile(max_rank));
        fflush(stdout);
      }
      prev_max_rank = max_rank;
    } while (n.GenerateRandomTile());

    gettimeofday(&finish, NULL);

    if (!options.quiet && !options.verbose) {
      putchar('\r');
      n.Show();
    }
    auto seconds = Elapse(start, finish);
    if (!options.quiet) {
      printf("game# %d moves %d seconds %.1f moves/s %.1f\n", options.seed + i,
             num_moves, seconds, num_moves / seconds);
    }
    if (print_search_metrics) {
      auto stats = Node::GetSearchStats();
      long long total_nodes = stats.move_nodes + stats.tile_nodes;
      double nodes_per_move =
          num_moves ? (double)total_nodes / num_moves : 0.0;
      long long lookups = Node::cache.LookupCount();
      long long hits = Node::cache.HitCount();
      double hit_rate = lookups ? (double)hits * 100.0 / lookups : 0.0;
      printf("search_metrics game=%d moves=%d total_nodes=%lld nodes_per_move=%.1f "
             "move_nodes=%lld tile_nodes=%lld tile_branches=%lld "
             "prune_score=%lld prune_depth=%lld prune_prob=%lld "
             "cache_lookups=%lld cache_hits=%lld cache_hit_rate=%.1f%%\n",
             options.seed + i, num_moves, total_nodes, nodes_per_move,
             stats.move_nodes, stats.tile_nodes, stats.tile_branches,
             stats.prune_score, stats.prune_depth, stats.prune_prob,
             lookups, hits, hit_rate);
    }

    int final_game_score = n.GameScore();
    int final_max_rank = n.MaxRank();
    int final_max_tile = n.MaxTile();
    int final_sum_tile = n.SumTile();

    sum_game_scores += final_game_score;
    ++max_rank_freq[final_max_rank];
    total_moves_logged += num_moves;
    if (!options.quiet && options.iterations > 1) {
      printf("%d-game average %ld max ", i + 1, sum_game_scores / (i + 1));
      for (int r = 0; r <= N * N; ++r)
        if (max_rank_freq[r])
          printf("%d*%d(%.1f%%) ", Node::Tile(r), max_rank_freq[r],
                 100.0 * max_rank_freq[r] / (i + 1));
      puts("");
    }

    if (enable_json_logging) {
      if (!step_logger.Close()) {
        fprintf(stderr, "Failed to finalize JSONL step log %s\n",
                steps_file_path.c_str());
        step_log_success = false;
      }
      if (step_log_success) {
        std::ofstream meta_stream(game_file_prefix + ".meta.json",
                                  std::ios::out | std::ios::trunc);
        if (!meta_stream) {
          fprintf(stderr, "Failed to write metadata for %s\n",
                  game_file_prefix.c_str());
        } else {
          meta_stream << '{';
          meta_stream << "\"seed\":" << options.seed + i << ',';
          meta_stream << "\"depth\":" << options.max_depth << ',';
          meta_stream << "\"game_index\":" << i << ',';
          meta_stream << "\"steps_file\":\"" << steps_file_path << "\",";
          meta_stream << "\"num_moves\":" << num_moves << ',';
          meta_stream << "\"score\":" << final_game_score << ',';
          meta_stream << "\"max_tile\":" << final_max_tile << ',';
          meta_stream << "\"max_rank\":" << final_max_rank << ',';
          meta_stream << "\"sum_tile\":" << final_sum_tile << ',';
          meta_stream << "\"seconds\":" << FormatFloat(seconds);
          meta_stream << '}';
        }
      }
    }

    if (options.progress_interval > 0 &&
        ((i + 1) % options.progress_interval) == 0) {
      fprintf(stderr,
              "[progress] %d/%d games complete (%.1f%%), total moves %lld\n",
              i + 1, options.iterations,
              options.iterations ? (100.0 * (i + 1) / options.iterations) : 0.0,
              total_moves_logged);
      fflush(stderr);
    }

    if (options.verbose && !options.quiet) Node::cache.ShowStats();
    if (!options.quiet) fflush(stdout);
  }
  return 0;
}
