#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType& key,
                              std::vector<ValueType>* result, Transaction* txn)
     ->  bool
{
  //Your code here
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID) {
    return false;
  }

  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);

  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();

  while (!tmp_page -> IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);

    if (slot_num == -1) {
      return false;
    }

    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }

  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);

  if (slot_num == -1 || comparator_(leaf_page -> KeyAt(slot_num), key) != 0) {
    return false;
  }

  result -> push_back(leaf_page -> ValueAt(slot_num));
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */


 /*
 总结一下insert的实现：
 1. 插入元素的块仍小于等于 order -> 直接插入
 2. 插入元素的块 大于order了 -> 裂成两块，前后块均order / 2 -> 把前块的最大值放到父块中，检查父块是否 > order
                                                          一路向上，直到根节点也 > order ,就把根也裂了，然后往上重建根节点           
 3. 特殊一点，插入元素比最大的元素都大了，就从根开始一路改成插入的这个元素然后再执行以上两种操作。                                                                             
 */

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn)  ->  bool
{
  //Your code here
  Context ctx;
  ctx.header_page_ = bpm_ -> FetchPageWrite(header_page_id_);
  auto& head_guard = *ctx.header_page_;
  ctx.root_page_id_ = head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_;

  // empty tree -> build tree
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    page_id_t new_page_id;
    auto new_page_guard = bpm_ -> NewPageGuarded(&new_page_id);
    auto* new_root = new_page_guard.template AsMut<LeafPage>();
    new_root -> Init(leaf_max_size_);
    new_root -> IncreaseSize(1);
    new_root -> SetKeyAt(0, key);
    new_root -> SetValueAt(0, value);
    head_guard.template AsMut<BPlusTreeHeaderPage>() -> root_page_id_ = new_page_id;
    return true;
  }

  WritePageGuard guard = bpm_ -> FetchPageWrite(ctx.root_page_id_);
  auto tmp_page = guard.template AsMut<BPlusTreePage>();

  while (!tmp_page -> IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1) {
      return false;
    }

    page_id_t child_page_id = internal -> ValueAt(slot_num);
    WritePageGuard child_guard = bpm_ -> FetchPageWrite(child_page_id);
    auto* child_page = child_guard.template AsMut<BPlusTreePage>();

    // Crab locking: if child has room, insert won't cause it to split,
    // so no structural change can propagate upward → release all ancestors
    if (child_page -> GetSize() < child_page -> GetMaxSize()) {
      ctx.write_set_.clear();
    }

    ctx.write_set_.push_back(std::move(guard));
    guard = std::move(child_guard);
    tmp_page = guard.template AsMut<BPlusTreePage>();
  }

  auto* leaf_page = reinterpret_cast<LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);

  if (slot_num != -1 && comparator_(leaf_page -> KeyAt(slot_num), key) == 0) {
    return false;
  }

  int idx = slot_num + 1;
  int oldsize = leaf_page -> GetSize();
  leaf_page -> IncreaseSize(1);

  for (int i = oldsize; i > idx; i--) {
    leaf_page -> SetKeyAt(i, leaf_page -> KeyAt(i - 1));
    leaf_page -> SetValueAt(i, leaf_page -> ValueAt(i - 1));
  }

  leaf_page -> SetKeyAt(idx, key);
  leaf_page -> SetValueAt(idx, value);

  int max_size = leaf_page -> GetMaxSize();
  int size = leaf_page -> GetSize();

  if (size <= max_size) return true;

  // consider split

  page_id_t new_page_id;
  auto new_page_guard = bpm_ -> NewPageGuarded(&new_page_id);
  auto new_page = new_page_guard.template AsMut<BPlusTreePage>();
  auto* back_page = reinterpret_cast<LeafPage*>(new_page);
  back_page -> Init(max_size);
  page_id_t old_page_id = guard.PageId();

  int front_size = size >> 1;
  int back_size = size - front_size;

  back_page -> SetSize(back_size);
  for (int i = 0; i < back_size; i++) {
    back_page -> SetKeyAt(i, leaf_page -> KeyAt(front_size + i));
    back_page -> SetValueAt(i, leaf_page -> ValueAt(front_size + i));
  }

  leaf_page -> SetSize(front_size);
  back_page -> SetNextPageId(leaf_page -> GetNextPageId());
  leaf_page -> SetNextPageId(new_page_id);

  auto new_key = back_page -> KeyAt(0);
  auto new_value = new_page_id;

  // only root

  if (ctx.write_set_.empty()) {
    page_id_t root_page_id;
    auto root_guard = bpm_ -> NewPageGuarded(&root_page_id);
    auto root_page = root_guard.template AsMut<InternalPage>();
    root_page -> Init(internal_max_size_);
    root_page -> SetValueAt(0, old_page_id);
    root_page -> SetKeyAt(1, new_key);
    root_page -> SetValueAt(1, new_value);
    root_page -> IncreaseSize(1);
    head_guard.template AsMut<BPlusTreeHeaderPage>() -> root_page_id_ = root_page_id;
    return true;
  }

  // upward split

  while(!ctx.write_set_.empty()) {
    auto father_guard = std::move(ctx.write_set_.back());
    ctx.write_set_.pop_back();
    auto father = father_guard.AsMut<BPlusTreePage>();
    auto* father_page = reinterpret_cast<InternalPage*>(father);
    int father_size = father_page -> GetSize();
    int father_max_size = father_page -> GetMaxSize();
    int pos = BinaryFind(father_page, new_key) + 1;
    father_page -> IncreaseSize(1);
    for (int i = father_size; i > pos; i--) {
      father_page -> SetKeyAt(i, father_page -> KeyAt(i - 1));
      father_page -> SetValueAt(i, father_page -> ValueAt(i - 1));
    }
    father_page -> SetKeyAt(pos, new_key);
    father_page -> SetValueAt(pos, new_value);
    if(father_page -> GetSize() <= father_max_size) return true;

    // split internal

    old_page_id = father_guard.PageId();
    new_page_guard = bpm_ -> NewPageGuarded(&new_page_id);
    auto* back_internal_page = new_page_guard.template AsMut<InternalPage>();
    back_internal_page -> Init(father_max_size);

    int current_father_size = father_page -> GetSize();
    front_size = current_father_size >> 1;
    back_size = current_father_size - front_size;

    back_internal_page -> SetValueAt(0, father_page -> ValueAt(front_size));
    back_internal_page -> SetSize(back_size);
    for (int i = 1; i < back_size; i++) {
      back_internal_page -> SetKeyAt(i, father_page -> KeyAt(front_size + i));
      back_internal_page -> SetValueAt(i, father_page -> ValueAt(front_size + i));
    }

    father_page -> SetSize(front_size);

    new_key = father_page -> KeyAt(front_size);
    new_value = new_page_id;
  }

  // renew root

  page_id_t root_page_id;
  auto root_guard = bpm_ -> NewPageGuarded(&root_page_id);
  auto root_page = root_guard.template AsMut<InternalPage>();
  root_page -> Init(internal_max_size_);
  root_page -> SetValueAt(0, old_page_id);
  root_page -> SetKeyAt(1, new_key);
  root_page -> SetValueAt(1, new_value);
  root_page -> IncreaseSize(1);
  head_guard.template AsMut<BPlusTreeHeaderPage>() -> root_page_id_ = root_page_id;

  return true;
}


/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */

 /*
 总结一下Remove的操作：
 1. 先往下一路找到底，如果删了这个叶子还是 >= order / 2，就不管
 2. 如果 < order / 2了，就找找兄弟，如果兄弟有多的，就借一个过来，顺便把父节点改一下
                                  如果没有多的，那正好两个直接合并了，记得改父节点。
                                  如果这一层（第二层）被合并到只剩一个节点了，就把根节点删了，彼可取而代之
 3. 如果删的数和键值一样记得改！ 
 */ 

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* txn)
{
  //Your code here

  Context ctx;
  ctx.header_page_ = bpm_ -> FetchPageWrite(header_page_id_);
  auto& head_guard = *ctx.header_page_;
  ctx.root_page_id_ = head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_;

  // empty tree
  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    return ;
  }

  // easy delete

  WritePageGuard guard = bpm_ -> FetchPageWrite(ctx.root_page_id_);
  auto tmp_page = guard.template AsMut<BPlusTreePage>();

  while (!tmp_page -> IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1) {
      return ;
    }

    page_id_t child_page_id = internal -> ValueAt(slot_num);
    WritePageGuard child_guard = bpm_ -> FetchPageWrite(child_page_id);
    auto* child_page = child_guard.template AsMut<BPlusTreePage>();

    // Crab locking: if child is more than half full, deleting below
    // won't cause it to underflow → no merge can propagate upward
    if (child_page -> GetSize() > child_page -> GetMinSize()) {
      ctx.write_set_.clear();
    }

    ctx.write_set_.push_back(std::move(guard));
    guard = std::move(child_guard);
    tmp_page = guard.template AsMut<BPlusTreePage>();
  }

  auto* leaf_page = reinterpret_cast<LeafPage*>(tmp_page);
  int slot_num = BinaryFind(leaf_page, key);

  if (slot_num == -1 || comparator_(leaf_page -> KeyAt(slot_num), key) != 0) {
    return ;
  }

  int idx = slot_num;
  int old_size = leaf_page -> GetSize();

  for (int i = idx; i < old_size - 1; i++) {
    leaf_page -> SetKeyAt(i, leaf_page -> KeyAt(i + 1));
    leaf_page -> SetValueAt(i, leaf_page -> ValueAt(i + 1));
  }

  leaf_page -> IncreaseSize(-1);
  int min_size = leaf_page -> GetMinSize();
  int size = leaf_page -> GetSize();

  if (size >= min_size) return ;

  // consider merge

  page_id_t current_page_id = guard.PageId();
  bool current_is_leaf = true;
  WritePageGuard current_guard = std::move(guard);

   while (1){
    auto current_page = current_guard.template AsMut<BPlusTreePage>();

    if (ctx.write_set_.empty()) {
      if (current_is_leaf) {
        if (current_page ->GetSize() == 0) {
          head_guard.template AsMut<BPlusTreeHeaderPage>() -> root_page_id_ = INVALID_PAGE_ID;
        }
        return ;
      }

      if (current_page -> GetSize() == 0) {
        head_guard.template AsMut<BPlusTreeHeaderPage>() ->root_page_id_ = INVALID_PAGE_ID;
      }
      else {
        head_guard.template AsMut<BPlusTreeHeaderPage>() ->root_page_id_ = current_page_id;
      }
      return ;
    }

    auto father_guard = std::move(ctx.write_set_.back());
    ctx.write_set_.pop_back();
    auto father = father_guard.template AsMut<BPlusTreePage>();
    auto* father_page = reinterpret_cast<InternalPage*>(father);
    int current_idx = -1;
    for (int i = 0; i < father_page -> GetSize(); i++) {
      if (father_page -> ValueAt(i) == current_page_id) {
        current_idx = i;
        break;
      }
    }
    int father_size = father_page -> GetSize();

    if (current_is_leaf) {
      auto* current_leaf = reinterpret_cast<LeafPage*>(current_page);

      // have left sibling

      if (current_idx > 0) {
        page_id_t left_page_id = father_page -> ValueAt(current_idx - 1);
        auto left_guard = bpm_ -> FetchPageWrite(left_page_id);
        auto* left_page = left_guard.template AsMut<LeafPage>();

        // left sibling has surplus elements

        if (left_page -> GetSize() > left_page -> GetMinSize()) {
          int current_size = current_leaf -> GetSize();
          int left_size = left_page -> GetSize();
          current_leaf -> IncreaseSize(1);
          for (int i = current_size; i > 0; i--) {
            current_leaf -> SetKeyAt(i, current_leaf -> KeyAt(i - 1));
            current_leaf -> SetValueAt(i, current_leaf -> ValueAt(i - 1));
          }
          current_leaf -> SetKeyAt(0, left_page -> KeyAt(left_size - 1));
          current_leaf -> SetValueAt(0, left_page -> ValueAt(left_size - 1));
          left_page -> IncreaseSize(-1);
          father_page -> SetKeyAt(current_idx, current_leaf -> KeyAt(0));
          return ;
        }
      }

      // don't have left sibling or left sibling has no surplus elements
      // but have right sibling

      if (current_idx + 1 < father_size) {
        page_id_t right_page_id = father_page -> ValueAt(current_idx + 1);
        auto right_guard = bpm_ -> FetchPageWrite(right_page_id);
        auto* right_page = right_guard.template AsMut<LeafPage>();

        // right sibling have surplus elements

        if (right_page -> GetSize() > right_page -> GetMinSize()) {
          int current_size = current_leaf -> GetSize();
          current_leaf -> SetKeyAt(current_size, right_page -> KeyAt(0));
          current_leaf -> SetValueAt(current_size, right_page -> ValueAt(0));
          current_leaf -> IncreaseSize(1);
          int right_size = right_page -> GetSize();
          for (int i = 0; i < right_size - 1; i++) {
            right_page -> SetKeyAt(i, right_page -> KeyAt(i + 1));
            right_page -> SetValueAt(i, right_page -> ValueAt(i + 1));
          }
          right_page -> IncreaseSize(-1);
          father_page -> SetKeyAt(current_idx + 1, right_page -> KeyAt(0));
          return ;
        }
      }

      // can' t borrow elements from left or right siblings
      // but have a left sibling
      // which means can be merged with left sibling
      if (current_idx > 0) {
        page_id_t left_page_id = father_page -> ValueAt(current_idx - 1);
        auto left_guard = bpm_ -> FetchPageWrite(left_page_id);
        auto* left_page = left_guard.template AsMut<LeafPage>();
        int left_size = left_page -> GetSize();
        int current_size = current_leaf -> GetSize();
        for (int i = 0; i < current_size; i++) {
          left_page -> SetKeyAt(left_size + i, current_leaf -> KeyAt(i));
          left_page -> SetValueAt(left_size + i, current_leaf -> ValueAt(i));
        }
        left_page -> IncreaseSize(current_size);
        left_page -> SetNextPageId(current_leaf -> GetNextPageId());
        for (int i = current_idx; i < father_size - 1; i++) {
          father_page -> SetKeyAt(i, father_page -> KeyAt(i + 1));
          father_page -> SetValueAt(i, father_page -> ValueAt(i + 1));
        }
        father_page -> IncreaseSize(-1);
      }
      // if don't have a left sibling, must have a right sibling
      // (otherwise this layer should be delete)
      else {
        page_id_t right_page_id = father_page -> ValueAt(current_idx + 1);
        auto right_guard = bpm_ -> FetchPageWrite(right_page_id);
        auto* right_page = right_guard.template AsMut<LeafPage>();
        int current_size = current_leaf -> GetSize();
        int right_size = right_page -> GetSize();
        for (int i = 0; i < right_size; i++) {
          current_leaf -> SetKeyAt(current_size + i, right_page -> KeyAt(i));
          current_leaf -> SetValueAt(current_size + i, right_page -> ValueAt(i));
        }
        current_leaf -> IncreaseSize(right_size);
        current_leaf -> SetNextPageId(right_page -> GetNextPageId());
        for (int i = current_idx + 1; i < father_size - 1; i++) {
          father_page -> SetKeyAt(i, father_page -> KeyAt(i + 1));
          father_page -> SetValueAt(i, father_page -> ValueAt(i + 1));
        }
        father_page -> IncreaseSize(-1);
      }

      current_page_id = father_guard.PageId();
      current_guard = std::move(father_guard);
      current_is_leaf = false;
      continue;
    }

    auto* current_internal = reinterpret_cast<InternalPage*>(current_page);

    if (current_internal -> GetSize() >= current_internal -> GetMinSize()) {
      return;
    }

    // Internal version

    if (current_idx > 0) {
      page_id_t left_page_id = father_page -> ValueAt(current_idx - 1);
      auto left_guard = bpm_ -> FetchPageWrite(left_page_id);
      auto* left_page = left_guard.template AsMut<InternalPage>();
      if (left_page -> GetSize() > left_page -> GetMinSize()) {
        int current_size = current_internal -> GetSize();
        int left_size = left_page -> GetSize();
        current_internal -> IncreaseSize(1);
        for (int i = current_size; i > 0; i--) {
          current_internal -> SetKeyAt(i, current_internal -> KeyAt(i - 1));
          current_internal -> SetValueAt(i, current_internal -> ValueAt(i - 1));
        }
        KeyType old_left_key = left_page -> KeyAt(left_size - 1);
        current_internal -> SetValueAt(0, left_page -> ValueAt(left_size - 1));
        current_internal -> SetKeyAt(1, father_page -> KeyAt(current_idx));
        left_page -> IncreaseSize(-1);
        father_page -> SetKeyAt(current_idx, old_left_key);
        return;
      }
    }

    if (current_idx + 1 < father_size) {
      page_id_t right_page_id = father_page -> ValueAt(current_idx + 1);
      auto right_guard = bpm_ -> FetchPageWrite(right_page_id);
      auto* right_page = right_guard.template AsMut<InternalPage>();
      if (right_page -> GetSize() > right_page -> GetMinSize()) {
        int current_size = current_internal -> GetSize();
        KeyType old_right_key_1 = right_page -> KeyAt(1);
        current_internal -> SetKeyAt(current_size, father_page -> KeyAt(current_idx + 1));
        current_internal -> SetValueAt(current_size, right_page -> ValueAt(0));
        current_internal -> IncreaseSize(1);
        int right_size = right_page -> GetSize();
        right_page -> SetValueAt(0, right_page -> ValueAt(1));
        for (int i = 1; i < right_size - 1; i++) {
          right_page -> SetKeyAt(i, right_page -> KeyAt(i + 1));
          right_page -> SetValueAt(i, right_page -> ValueAt(i + 1));
        }
        right_page -> IncreaseSize(-1);
        father_page -> SetKeyAt(current_idx + 1, old_right_key_1);
        return;
      }
    }

    if (current_idx > 0) {
      page_id_t left_page_id = father_page -> ValueAt(current_idx - 1);
      auto left_guard = bpm_ -> FetchPageWrite(left_page_id);
      auto* left_page = left_guard.template AsMut<InternalPage>();
      int left_size = left_page -> GetSize();
      int current_size = current_internal -> GetSize();
      left_page -> SetKeyAt(left_size, father_page -> KeyAt(current_idx));
      left_page -> SetValueAt(left_size, current_internal -> ValueAt(0));
      for (int i = 1; i < current_size; i++) {
        left_page -> SetKeyAt(left_size + i, current_internal -> KeyAt(i));
        left_page -> SetValueAt(left_size + i, current_internal -> ValueAt(i));
      }
      left_page -> IncreaseSize(current_size);
      for (int i = current_idx; i < father_size - 1; i++) {
        father_page -> SetKeyAt(i, father_page -> KeyAt(i + 1));
        father_page -> SetValueAt(i, father_page -> ValueAt(i + 1));
      }
      father_page -> IncreaseSize(-1);
    }
    else {
      page_id_t right_page_id = father_page -> ValueAt(current_idx + 1);
      auto right_guard = bpm_ -> FetchPageWrite(right_page_id);
      auto* right_page = right_guard.template AsMut<InternalPage>();
      int current_size = current_internal -> GetSize();
      int right_size = right_page -> GetSize();
      current_internal -> SetKeyAt(current_size, father_page -> KeyAt(current_idx + 1));
      current_internal -> SetValueAt(current_size, right_page -> ValueAt(0));
      for (int i = 1; i < right_size; i++) {
        current_internal -> SetKeyAt(current_size + i, right_page -> KeyAt(i));
        current_internal -> SetValueAt(current_size + i, right_page -> ValueAt(i));
      }
      current_internal -> IncreaseSize(right_size);
      for (int i = current_idx + 1; i < father_size - 1; i++) {
        father_page -> SetKeyAt(i, father_page -> KeyAt(i + 1));
        father_page -> SetValueAt(i, father_page -> ValueAt(i + 1));
      }
      father_page -> IncreaseSize(-1);
    }

    current_page_id = father_guard.PageId();
    current_guard = std::move(father_guard);
    current_is_leaf = false;
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
     ->  int
{
  int l = 0;
  int r = leaf_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(leaf_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r >= 0 && comparator_(leaf_page -> KeyAt(r), key) == 1)
  {
    r = -1;
  }

  return r;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key)  ->  int
{
  int l = 1;
  int r = internal_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(internal_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r == -1 || comparator_(internal_page -> KeyAt(r), key) == 1)
  {
    r = 0;
  }

  return r;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = root_header_page -> root_page_id_;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub