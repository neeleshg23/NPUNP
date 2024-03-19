#include "SystolicWS.h"

SystolicWS::SystolicWS(uint32_t id, SimulationConfig config)
    : Core(id, config) {}

void SystolicWS::cycle() {
  /*
  Compute unit
  */
  if (!_compute_pipeline.empty() &&
      _compute_pipeline.front().finish_cycle <= _core_cycle) {
    Instruction inst = _compute_pipeline.front();
    if (inst.dest_addr >= ACCUM_SPAD_BASE)
      _acc_spad.fill(inst.dest_addr, inst.accum_spad_id);
    else
      assert(0);
    _compute_pipeline.pop();
  }

  /* Checking Vector compute pipeline */
  if (!_vector_pipeline.empty() &&
      _vector_pipeline.front().finish_cycle <= _core_cycle) {
    Instruction inst = _vector_pipeline.front();
    if (inst.dest_addr >= ACCUM_SPAD_BASE) {
      _acc_spad.fill(inst.dest_addr, inst.accum_spad_id);
    } else
      assert(0);
    _vector_pipeline.pop();
  }
  /* LD in struction queue */
  if (!_ld_inst_queue.empty()) {
    Instruction front = _ld_inst_queue.front();
    if (front.opcode == Opcode::MOVIN) {
      bool prefetched = false;
      Sram *buffer;
      int buffer_id;
      if (front.dest_addr >= ACCUM_SPAD_BASE) {
        buffer = &_acc_spad;
        buffer_id = front.accum_spad_id;
      } else {
        buffer = &_spad;
        buffer_id = front.spad_id;
      }

      buffer->prefetch(front.dest_addr, buffer_id, front.size, front.size);
      for (addr_type addr : front.src_addrs) {
        assert(front.base_addr != GARBEGE_ADDR);
        MemoryAccess *access =
            new MemoryAccess({.id = generate_mem_access_id(),
                              .dram_address = addr + front.base_addr,
                              .spad_address = front.dest_addr,
                              .size = _config.dram_req_size,
                              .write = false,
                              .request = true,
                              .core_id = _id,
                              .start_cycle = _core_cycle,
                              .buffer_id = buffer_id});
        _request_queue.push(access);
      }
      _ld_inst_queue.pop();
    } else {
      assert(0);
    }
  }
  /* ST in struction queue */
  if (!_st_inst_queue.empty()) {
    Instruction front = _st_inst_queue.front();
    if (front.opcode == Opcode::MOVOUT || front.opcode == Opcode::MOVOUT_POOL) {
      Sram *buffer;
      int buffer_id;
      if (front.dest_addr >= ACCUM_SPAD_BASE) {
        buffer = &_acc_spad;
        buffer_id = front.accum_spad_id;
      } else {
        buffer = &_spad;
        buffer_id = front.spad_id;
      }
      assert(buffer->check_hit(front.dest_addr, buffer_id));
      for (addr_type addr : front.src_addrs) {
        assert(front.base_addr != GARBEGE_ADDR);
        MemoryAccess *access =
            new MemoryAccess{.id = generate_mem_access_id(),
                             .dram_address = addr + front.base_addr,
                             .spad_address = front.dest_addr,
                             .size = _config.dram_req_size,
                             .write = true,
                             .request = true,
                             .core_id = _id,
                             .start_cycle = _core_cycle,
                             .buffer_id = buffer_id};
        _waiting_write_reqs++;
        _request_queue.push(access);
      }
      _st_inst_queue.pop();
    } else {
      assert(0);
    }
  }
  /* EX instruction queue */
  if (!_ex_inst_queue.empty() && can_issue_compute(_ex_inst_queue.front())) { // execution dependecy check
    Instruction front = _ex_inst_queue.front();
    if (front.opcode == Opcode::GEMM || front.opcode == Opcode::GEMM_PRELOAD) {
      assert(can_issue_compute(front));
      if (!_compute_pipeline.empty()) {
        /* Preload can be hided */
        uint32_t offset = _compute_pipeline.back().size;
        offset = MAX(offset, 4);
        if (front.opcode == Opcode::GEMM_PRELOAD) {
          // State mul-pre
          offset = _config.core_height;
          _stat_systolic_preload_issue_count++;
        }
        front.start_cycle = _compute_pipeline.back().start_cycle + offset;
      } else {
        front.start_cycle = _core_cycle;
        /* Preload weight to systolic array*/
        if (front.opcode == Opcode::GEMM_PRELOAD) {
          /* Weight preload  from buffer latecny + WEight preload latency */
          front.start_cycle += _config.core_height + _config.core_height - 1;
          _stat_systolic_preload_issue_count++;
        }
      }

      front.finish_cycle = front.start_cycle + get_inst_compute_cycles(front);
      _compute_pipeline.push(front);
      _stat_systolic_inst_issue_count++;
    } else if (front.opcode == Opcode::COMP || front.opcode == Opcode::SOFTMAX ||
               front.opcode == Opcode::IM2COL || front.opcode == Opcode::LAYERNORM ||
               front.opcode == Opcode::ADD || front.opcode == Opcode::GELU) {  // vector unit compute
      assert(can_issue_compute(front));           // check dependencys in SRAM
      if (!_vector_pipeline.empty()) {
        front.start_cycle =
            _vector_pipeline.back().start_cycle + _vector_pipeline.back().size;
      } else {
        front.start_cycle = _core_cycle;
      }
      front.finish_cycle =
          front.start_cycle +
          get_vector_compute_cycles(front);  // Setting IC as 1 (Might need to modify)
      _vector_pipeline.push(front);
    }
    _ex_inst_queue.pop();
    if (_acc_spad.check_allocated(front.dest_addr, front.accum_spad_id)) {
      _acc_spad.count_up(front.dest_addr, front.accum_spad_id);
    } else {
      _acc_spad.prefetch(front.dest_addr, front.accum_spad_id, front.size, 1);
    }
  }

  if (_compute_pipeline.empty() && _ex_inst_queue.empty()) {
    _stat_memory_cycle++;
  } else if (!_compute_pipeline.empty()) {
    _stat_compute_cycle++;
  }
  if (!_vector_pipeline.empty()) {  // Vector unit compute
    _stat_vec_compute_cycle++;
  }

  if (!running()) {
    _stat_idle_cycle++;
  }
  Core::cycle();
}

cycle_type SystolicWS::get_inst_compute_cycles(Instruction inst) {
  return _config.core_height + _config.core_width - 2 + MAX(inst.size, 4);
}

cycle_type SystolicWS::calculate_add_tree_iterations(uint32_t vector_size) {
  uint32_t calculation_unit = _config.vector_process_bit >> 3;
  if (vector_size <= calculation_unit) {
    return 1;
  }

  uint32_t ret = vector_size / calculation_unit;
  if (vector_size % calculation_unit != 0) {
    ret++;
  }
  return ret + calculate_add_tree_iterations(ret);
}

cycle_type SystolicWS::calculate_vector_op_iterations(uint32_t vector_size) {
  uint32_t calculation_unit = _config.vector_process_bit >> 3;
  uint32_t ret = vector_size / calculation_unit;
  if (vector_size % calculation_unit != 0) {
    ret++;
  }
  return ret;
}

cycle_type SystolicWS::get_vector_compute_cycles(Instruction &inst) {
  cycle_type vec_op_iter = calculate_vector_op_iterations(inst.size);
  cycle_type add_tree_iter = calculate_add_tree_iterations(inst.size);
  cycle_type add_tree, scalar_ops, vector_ops;
  switch (inst.opcode) {
    case Opcode::LAYERNORM:
      add_tree = 2 * add_tree_iter * _config.add_tree_latency;
      scalar_ops = 2 * _config.scalar_mul_latency + _config.scalar_sqrt_latency;
      // 1 addition, 1 subtraction, 1 division, 2 multiplication.
      vector_ops = vec_op_iter * (2 * _config.add_latency + 3 * _config.mul_latency) * inst.tile_m;
      return add_tree + scalar_ops + vector_ops;
    case Opcode::SOFTMAX:
      // 1 add tree, 1 compare tree
      add_tree = 2 * add_tree_iter * _config.add_tree_latency * inst.tile_m;
      vector_ops =
        vec_op_iter * (_config.add_latency + _config.exp_latency + _config.mul_latency);
      return add_tree + vector_ops;
    case Opcode::ADD:
      return vec_op_iter * _config.add_latency;
    case Opcode::GELU:
      return vec_op_iter * _config.gelu_latency;
    case Opcode::COMP:
      return vec_op_iter * 1;
  }
  spdlog::info("not configured operation. {}", inst.id);
  // assert(0);
  return 0;
}

void SystolicWS::print_stats() {
  Core::print_stats();
  spdlog::info("Core [{}] : Systolic Inst Issue Count : {}", _id,
               _stat_systolic_inst_issue_count);
  spdlog::info("Core [{}] : Systolic PRELOAD Issue Count : {}", _id,
               _stat_systolic_preload_issue_count);
}