/*
 * Project: curve
 * Created Date: Wed Nov 28 2018
 * Author: lixiaocui
 * Copyright (c) 2018 netease
 */

#include <cstdint>
#include <vector>
#include "src/mds/schedule/operator.h"
#include "src/mds/schedule/topoAdapter.h"
#include "src/mds/schedule/operatorStep.h"

#ifndef SRC_MDS_SCHEDULE_OPERATORFACTORY_H_
#define SRC_MDS_SCHEDULE_OPERATORFACTORY_H_
namespace curve {
namespace mds {
namespace schedule {

class OperatorFactory {
 public:
  /**
   * @brief generate operator to transfer leader
   */
  Operator CreateTransferLeaderOperator(const CopySetInfo &info,
                                        ChunkServerIdType newLeader,
                                        OperatorPriority pri);

  /**
   * @brief  generate operator to safely remove peer
   */
  Operator CreateRemovePeerOperator(
      const CopySetInfo &info, ChunkServerIdType rmPeer, OperatorPriority pri);

  /**
   * @brief generate operator to add peer
   */
  Operator CreateAddPeerOperator(
      const CopySetInfo &info, ChunkServerIdType addPeer, OperatorPriority pri);
};

extern OperatorFactory operatorFactory;

}  // namespace schedule
}  // namespace mds
}  // namespace curve
#endif  // SRC_MDS_SCHEDULE_OPERATORFACTORY_H_
