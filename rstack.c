// ARCHITECTURE NOTE:
// This implementation utilizes reachability-based Garbage Collection (GC)
// starting from a root node (SUPER_STACK). This approach natively and
// safely resolves the deallocation of cyclic graph structures without
// leaking memory, while maintaining O(1) amortized cleanup time.

#include "rstack.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// This enum type is used for controlling the behavior of the dfs function.
// Used as a return type of `preorder_fn` passed to dfs function.
typedef enum dfs_command {
  DFS_PROCESS_CHILDREN, // Commands to iterate through children and recurse.
  DFS_RETURN,           // Commands to terminate the current dfs stack frame.
  DFS_CASCADE_RETURN    // Commands to terminate all dfs stack frames.
} dfs_command;

// This enum type represents node's state during graph traversal.
typedef enum node_state {
  NODE_STATE_UNVISITED,
  NODE_STATE_VISITED,
  NODE_STATE_ACTIVE,
} node_state;

// Double-ended list node.
typedef struct list_node {
  struct list_node *next[2];
  rstack_t *stack_ptr;
} list_node;

// Used for indexing double ended lists' heads.
constexpr size_t FRONT = 0;
constexpr size_t BACK = 1;

struct rstack {
  node_state _state; // Used for storing node's state during graph traversals.
  bool alive;        // `false` if `delete_rstack` was called on this rstack
                     // and `true` otherwise.

  bool holds_value; // If `holds_value` is `false`, then `head[FRONT/BACK]`
                    // refers to the `FRONT/BACK` head of the list of stacks
                    // that were put on top of `this` stack. Otherwise, `value`
                    // holds the value `this` stack represents.

  union {
    list_node *head[2];
    uint64_t value;
  };
};

// Helper structure used for handling global state during dfs traversal.
// Used in `rstack_write` implementation.
typedef struct write_metadata {
  int write_errno;
  bool cycle_found;
  FILE *file;
} write_metadata;

// Structure used for holding the state of the memory management system.
// This information is held to enable efficient garbage collection.
struct rstack_manager {
  // Each newly created rstack is appended to `super_stack`.
  rstack_t super_stack;
  size_t current_potential; // Metadata used by garbage collector.
                            // Equal to: collective number of instances
                            // of `rstack` and `list_node` currently allocated

  size_t last_potential; // Metadata used by garbage collector.
                         // Equal to `current_potential` value at the moment
                         // previous full garbage collector pass was completed.

  size_t deleted_rstacks; // Number of stacks held in memory on which
                          // `delete_rstack` has been called.

  size_t allocated_rstacks; // number of currently allocated `rstack` instances
};

static struct rstack_manager manager = {
    .super_stack = {._state = NODE_STATE_UNVISITED,
                    .alive = false,
                    .holds_value = false,
                    .head = {nullptr, nullptr}}
    // Other members are auto-zeroed.
};

static rstack_t *const SUPER_STACK = &manager.super_stack;

// Allocates and initializes a new `list_node` instance it.
static list_node *list_node_new() {
  list_node *ptr = malloc(sizeof(list_node));

  if (ptr == nullptr) {
    return nullptr;
  }

  // Updating garbage collector's metadata.
  ++manager.current_potential;

  ptr->next[FRONT] = ptr->next[BACK] = nullptr;
  ptr->stack_ptr = nullptr;
  return ptr;
}

// Assumes `lst` points to a valid list_node object.
// Unlinks `lst` the list it belongs to and connects `lst's` neighbours.
// The function DOES NOT free `lst's` memory or modify `lst` in any way.
static void list_node_unlink(list_node const *l) {
  assert(l);

  list_node *next = l->next[FRONT];
  list_node *prev = l->next[BACK];

  if (next) {
    next->next[BACK] = prev;
  }
  if (prev) {
    prev->next[FRONT] = next;
  }
}

// Frees `lst` after unlinking it from the list it belongs to.
static void list_node_dealloc(list_node *lst) {
  list_node_unlink(lst);

  // Updating garbage collector's metadata.
  --manager.current_potential;

  free(lst);
}

// Assumes `rstack_delete` has been called on `rs`.
// Frees `rs` and memory allocated for the list it owns.
static void rstack_dealloc(rstack_t *rs) {
  assert(!rs->alive);

  if (!rs->holds_value) {
    for (list_node *l = rs->head[FRONT]; l != nullptr;) {
      list_node *next = l->next[FRONT];
      list_node_dealloc(l);
      l = next;
    }
  }

  // Updating garbage collector's metadata.
  --manager.deleted_rstacks;
  --manager.allocated_rstacks;
  --manager.current_potential;

  free(rs);
}

// Assumes `rs` points to a valid rstack instance.
// Appends `list_node` `lst` to the front of `rs's` list of neighbours.
static void rstack_push(rstack_t *rs, list_node *lst) {
  assert(rs && lst);

  if (!rs->head[FRONT]) {
    rs->head[FRONT] = rs->head[BACK] = lst;
  }
  else {
    rs->head[FRONT]->next[BACK] = lst;
    lst->next[FRONT] = rs->head[FRONT];
    rs->head[FRONT] = lst;
  }
}

// Allocates a new `rstack` instance holding `value`,
// and initializes it accordingly.
static rstack_t *rstack_new_from_value(uint64_t value) {
  rstack_t *ptr = rstack_new();

  if (!ptr) {
    return nullptr;
  }

  ptr->holds_value = true;
  ptr->value = value;

  return ptr;
}

// Allocates and initializes a new `rstack` instance.
rstack_t *rstack_new() {
  rstack_t *ptr = malloc(sizeof(rstack_t));

  if (ptr == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }

  ptr->head[FRONT] = ptr->head[BACK] = nullptr;
  ptr->_state = NODE_STATE_UNVISITED;
  ptr->holds_value = false;
  ptr->alive = true;

  if (rstack_push_rstack(SUPER_STACK, ptr) == -1) {
    free(ptr);
    return nullptr;
  }

  // Updating garbage collector's metadata.
  ++manager.allocated_rstacks;
  ++manager.current_potential;

  return ptr;
}

// Adds `rs2` to the front of `rs1's` list of neighbours.
int rstack_push_rstack(rstack_t *rs1, rstack_t *rs2) {
  if (rs1 == nullptr || rs2 == nullptr) {
    errno = EINVAL;
    return -1;
  }

  list_node *lst = list_node_new();

  if (lst == nullptr) {
    return -1;
  }

  lst->stack_ptr = rs2;
  rstack_push(rs1, lst);

  return 0;
}

// Creates an rstack from `value` and adds
// it to the front of `rs's` list of neighbours.
int rstack_push_value(rstack_t *rs1, uint64_t value) {
  rstack_t *ptr = rstack_new_from_value(value);

  if (!ptr) {
    return -1;
  }

  int r = rstack_push_rstack(rs1, ptr);
  rstack_delete(ptr);

  return r;
}

// A generic graph traversal function.
// `v`            - Node being currently visited.
// `side`         - The direction of stack traversal (pass FRONT or BACK).
//                  FRONT means top to bottom and BACK means bottom to top.
// `preorder_fn`  - A function to be executed when entering a node.
//                  Arbitrary computation can be performed inside.
//                  It should return a `dfs_command` that decides how the dfs
//                  function should proceed the graph traversal.
//                  Its first argument is the node being currently visited.
//                  Its second argument is a pointer `data` (described below).
// `postorder_fn` - A function to be executed on a node in post-order manner.
//                  Arbitrary computation can be performed inside.
//                  Its first argument is the node being currently visited.
//                  Its second argument is a pointer `data` (described below).
// `data`         - A pointer to some object, which can be written to and read
//                  from by `preorder_fn` and `postorder_fn` to maintain state.
//
// Returns `true` to propagate a `DFS_CASCADE_RETURN` abort signal up the call
// stack.
static bool dfs(rstack_t *v, size_t side, void *data,
                dfs_command (*preorder_fn)(rstack_t *v, void *),
                void (*postorder_fn)(rstack_t *v, void *)) {
  if (preorder_fn) {
    dfs_command cmd = preorder_fn(v, data);

    if (cmd == DFS_CASCADE_RETURN) {
      return true;
    }
    else if (cmd == DFS_RETURN) {
      return false;
    }
  }

  if (!v->holds_value) {
    for (list_node *lst = v->head[side]; lst != nullptr;
         lst = lst->next[side]) {
      rstack_t *s = lst->stack_ptr;

      if (dfs(s, side, data, preorder_fn, postorder_fn)) {
        return true;
      }
    }
  }

  if (postorder_fn) {
    postorder_fn(v, data);
  }

  return false;
}

// `preorder_fn` used for marking reachable nodes in dfs traversal.
// Used in the implementation of `collect_garbage`.
static dfs_command mark_reachable(rstack_t *v, void *) {
  if (v->_state) {
    return DFS_RETURN;
  }
  v->_state = NODE_STATE_VISITED;
  return DFS_PROCESS_CHILDREN;
}

// `preorder_fn` used for setting graph's metadata back to `UNVISITED` state.
static dfs_command unmark_reachable(rstack_t *rs, void *) {
  if (rs->_state == NODE_STATE_UNVISITED) {
    return DFS_RETURN;
  }
  rs->_state = NODE_STATE_UNVISITED;
  return DFS_PROCESS_CHILDREN;
}

// `preorder_fn` used for setting graph's metadata back to `UNVISITED` state.
// Optimized not to break `rstack_front's` time complexity during clear phase.
static dfs_command rstack_front_unmark_reachable(rstack_t *rs, void *) {
  if (rs->holds_value) {
    rs->_state = NODE_STATE_UNVISITED;
    return DFS_CASCADE_RETURN;
  }
  if (rs->_state == NODE_STATE_UNVISITED) {
    return DFS_RETURN;
  }
  rs->_state = NODE_STATE_UNVISITED;
  return DFS_PROCESS_CHILDREN;
}

// `preorder_fn` used for retrieving the front-most value in dfs traversal.
// Used in the implementation of `rstack_front`.
static dfs_command retrieve_val(rstack_t *rs, void *data) {
  if (rs->_state) {
    return DFS_RETURN;
  }

  rs->_state = NODE_STATE_VISITED;

  if (((result_t *)data)->flag == false) {
    if (rs->holds_value) {
      ((result_t *)data)->flag = true;
      ((result_t *)data)->value = rs->value;
    }
  }

  if (((result_t *)data)->flag) {
    return DFS_CASCADE_RETURN;
  }

  return DFS_PROCESS_CHILDREN;
}

// `preorder_fn` used for writing stack's values to a file determined by
// `((write_metadata *)wm)->file` file pointer.
// Used in the implementation of `rstack_write`.
static dfs_command write_value(rstack_t *v, void *wm) {
  if (v->_state == NODE_STATE_ACTIVE || ((write_metadata *)wm)->cycle_found) {
    ((write_metadata *)wm)->cycle_found = true;
    return DFS_CASCADE_RETURN;
  }

  v->_state = NODE_STATE_ACTIVE;

  if (((write_metadata *)wm)->write_errno) {
    return DFS_CASCADE_RETURN;
  }
  if (v->holds_value) {
    if (fprintf(((write_metadata *)wm)->file, "%" PRIu64 "\n", v->value) < 0) {
      ((write_metadata *)wm)->write_errno = errno;
    }
  }

  if (((write_metadata *)wm)->write_errno) {
    return DFS_CASCADE_RETURN;
  }
  else {
    return DFS_PROCESS_CHILDREN;
  }
}

// `postorder_fn` used for managing metadata required to
// correctly recognize a cyclic graph during dfs traversal.
// Works together with `write_value` preorder_fn`.
// Used in the implementation of `rstack_write`.
static void write_postorder(rstack_t *v, void *) {
  v->_state = NODE_STATE_VISITED;
}

// Tracks and deallocates memory that is longer needed,
// including cyclic structures formed of deleted rstacks.
// Time complexity - amortized O(1).
static void collect_garbage() {
  // Time complexity amortization mechanism.
  if (manager.deleted_rstacks != manager.allocated_rstacks &&
      manager.current_potential < 2 * manager.last_potential) {
    return;
  }

  // Assessing reachability from nondeleted rstacks.
  for (list_node *lst = SUPER_STACK->head[FRONT]; lst != nullptr;
       lst = lst->next[FRONT]) {
    rstack_t *ptr = lst->stack_ptr;

    if (ptr->alive) {
      dfs(ptr, FRONT, nullptr, mark_reachable, nullptr);
    }
  }

  // Deallocating unreachable rstacks.
  for (list_node *lst = SUPER_STACK->head[FRONT]; lst != nullptr;) {
    rstack_t *ptr = lst->stack_ptr;
    list_node *next = lst->next[FRONT];

    if (ptr->_state == NODE_STATE_UNVISITED) {
      // Ensuring no dangling references to `lst` remain in SUPER_STACK's list.
      if (lst == SUPER_STACK->head[FRONT]) {
        SUPER_STACK->head[FRONT] = lst->next[FRONT];
      }

      if (lst == SUPER_STACK->head[BACK]) {
        SUPER_STACK->head[BACK] = lst->next[BACK];
      }

      list_node_dealloc(lst);
      rstack_dealloc(ptr);
    }
    else {
      ptr->_state = NODE_STATE_UNVISITED;
    }

    lst = next;
  }

  manager.last_potential = manager.current_potential;
}

void rstack_pop(rstack_t *rs) {
  if (rs == nullptr || !rs->head[FRONT]) {
    return;
  }

  list_node *lst = rs->head[FRONT];
  rs->head[FRONT] = lst->next[FRONT];

  // If the list is now empty, clear the BACK pointer as well
  if (rs->head[FRONT] == nullptr) {
    rs->head[BACK] = nullptr;
  }

  list_node_dealloc(lst);
  collect_garbage();
}

// Marks the stack as dead. Memory is lazilly freed by the garbage
// collector, once no longer reachable from alive stacks.
void rstack_delete(rstack_t *rs) {
  if (rs == nullptr) {
    return;
  }

  // Updating garbage collector's metadata.
  ++manager.deleted_rstacks;
  rs->alive = false;

  collect_garbage();
}

bool rstack_empty(rstack_t *rs) {
  // Supplied `rstack_front` implementation is guaranteed to find the
  // frontmost value on stack `rs` regardless of the existence of cycles.
  // The following condition is thus equivalent to `rs's` emptiness.
  return (rstack_front(rs).flag == false);
}

rstack_t *rstack_read(char const *path) {
  if (path == nullptr) {
    errno = EINVAL;
    return nullptr;
  }

  int fd = open(path, O_RDONLY);

  if (fd == -1) {
    return nullptr;
  }

  rstack_t *rs = rstack_new();

  uint64_t current_val = 0;
  ssize_t result = 0;
  bool currently_on_digit = false;

  if (rs != nullptr) {
    constexpr size_t BUF_SIZE = 4096;
    static char buf[BUF_SIZE];

    while ((result = read(fd, buf, BUF_SIZE)) > 0) {
      for (size_t i = 0; i < (size_t)result; i++) {
        char c = buf[i];

        if (!isdigit(c) && !isspace(c)) {
          result = -1;
          errno = EINVAL;
          break;
        }

        if (isdigit(c)) {
          // Making sure value being read lies in uint64_t range.
          if (current_val > UINT64_MAX / 10 ||
              (current_val == UINT64_MAX / 10 &&
               (uint64_t)(c - '0') > UINT64_MAX % 10)) {
            result = -1;
            errno = EINVAL;
            break;
          }
          else {
            current_val = 10 * current_val + (c - '0');
          }

          currently_on_digit = true;
        }
        else if (currently_on_digit) {
          int push_result = rstack_push_value(rs, current_val);

          if (push_result == -1) {
            result = -1;
            break;
          }

          current_val = 0;
          currently_on_digit = false;
        }
      }

      if (result == -1) {
        break;
      }
    }
  }
  else {
    result = -1;
  }

  int current_errno = errno;
  int close_result = close(fd);

  if (result == -1) {
    errno = current_errno;
    rstack_delete(rs);
    return nullptr;
  }

  if (close_result == -1) {
    rstack_delete(rs);
    return nullptr;
  }

  if (currently_on_digit) {
    int push_result = rstack_push_value(rs, current_val);

    if (push_result == -1) {
      rstack_delete(rs);
      return nullptr;
    }
  }

  return rs;
}

result_t rstack_front(rstack_t *rs) {
  if (rs == nullptr) {
    return (result_t){false, 0};
  }

  result_t result = {.flag = false, .value = 0};
  dfs(rs, FRONT, &result, retrieve_val, nullptr);
  dfs(rs, FRONT, nullptr, rstack_front_unmark_reachable, nullptr);

  return result;
}

int rstack_write(char const *path, rstack_t *rs) {
  if (path == nullptr || rs == nullptr) {
    errno = EINVAL;
    return -1;
  }

  FILE *file = fopen(path, "w");

  if (file == nullptr) {
    return -1;
  }

  write_metadata wm = {.write_errno = 0, .cycle_found = false, .file = file};

  // The following call recursively retrieves values from `rs` in `BACK` order
  // (i.e. from stack's bottom to its top) and writes them to file `wm->file`.
  dfs(rs, BACK, &wm, write_value, write_postorder);
  dfs(rs, BACK, nullptr, unmark_reachable, nullptr);

  int close_result = fclose(file);

  if (wm.write_errno != 0) {
    errno = wm.write_errno;
    return -1;
  }

  if (close_result == -1) {
    return -1;
  }

  return 0;
}