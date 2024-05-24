import logging
import torch
import os
import time
import threading
import traceback
import asyncio
from typing import List, Any, Union
from maga_transformer.config.gpt_init_model_parameters import GptInitModelParameters
from maga_transformer.distribute.worker_info import g_parallel_info
from maga_transformer.metrics import GaugeMetrics, kmonitor
from maga_transformer.utils.time_util import Timer

from maga_transformer.models.base_model import BaseModel
from maga_transformer.ops.comm.nccl_op import NcclOp
from maga_transformer.async_decoder_engine.embedding.embedding_scheduler import EmbeddingScheduler
from maga_transformer.async_decoder_engine.embedding.embedding_model_executor import EmbeddingModelExecutor
from maga_transformer.async_decoder_engine.embedding.embedding_stream import EmbeddingStream, EmbeddingBatchedInput, EngineInputs, EngineOutputs

class EmbeddingDecoderEngine(object):
    def __init__(self, config: GptInitModelParameters, model: BaseModel):
        self.config_ = config
        self.batch_input_ = EmbeddingBatchedInput(NcclOp())
        self.scheduler_ = EmbeddingScheduler(self.config_)
        self.executor_ = EmbeddingModelExecutor(model, config)

    async def decode(self, inputs: EngineInputs) -> EngineOutputs:
        stream = self.scheduler_.enqueue(inputs)
        return await self._generate_loop(stream)

    async def _generate_loop(self, stream: EmbeddingStream) -> EngineOutputs:
        while True:
            if stream.error_info != None:
                raise Exception(stream.error_info)
            if stream.finished:
                break
            await asyncio.sleep(0.001)
        assert stream.outputs is not None, "stream output should not be None"
        return stream.outputs

    @torch.inference_mode()
    def step(self):
        streams = []
        try:
            with Timer() as t:
                streams = self.scheduler_.schedule()
                if len(streams) == 0 and g_parallel_info.tp_rank == 0:
                    torch.cuda.nvtx.range_pop()
                    time.sleep(0.001)
                    return
                self.batch_input_.generate_model_input(streams)
                embedding_outputs = self.executor_.process(self.batch_input_)
            if g_parallel_info.tp_rank == 0:
                # do synchronize before update result
                torch.cuda.synchronize()
                if self.batch_input_.batch_size != len(embedding_outputs):
                    raise Exception(f"batch size not equal to output length, {self.batch_input_.batch_size} vs {len(embedding_outputs)}")
                self.update_output(streams, embedding_outputs)
                self.report_metric(self.scheduler_.wait_queue_size(), len(streams), t.cost_ms())
        except Exception as e:
            for stream in streams:
                stream.set_error(f'{e}, Traceback: {traceback.format_exc()}')
            if (g_parallel_info.tp_size) > 1 or ("CUDA" in str(e)):
                kmonitor.report(GaugeMetrics.ERROR_EXIT_METRIC, 1)
                kmonitor.flush()
                time.sleep(0.1)
                # NOTE: nccl could hang when any error. GPU may hang under CUDA error.
                os._exit(-1)

    def update_output(self, streams: List[EmbeddingStream], embedding_outputs: Union[torch.Tensor, List[Any]]):
        bias = 0
        for stream in streams:
            stream.update(embedding_outputs[bias: bias + stream.inputs.batch_size])
            bias += stream.inputs.batch_size

    def report_metric(self, wait_size: int, batch_size: int, cost_time: float):
        kmonitor.report(GaugeMetrics.ASYNC_WAIT_QUERY_SIZE_METRIC, wait_size)
        kmonitor.report(GaugeMetrics.ASYNC_BATCH_SIZE_METRIC, batch_size)
        kmonitor.report(GaugeMetrics.ASYNC_ITERATE_LANTENCY, cost_time)

    def start(self):
        self.need_stop_ = False
        self.thread = threading.Thread(target=self.run_engine, daemon=True)
        self.thread.start()

    def stop(self):
        logging.info("decoder engine begin stop")
        self.need_stop_ = True
        self.thread.join()
        logging.info("decoder engine stop done")


    def run_engine(self):
        while not self.need_stop_:
            self.step()
        logging.info("need stop flag is true, exit run_engine")
