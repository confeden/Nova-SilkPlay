"""Static-shape RIFE reference through the official TensorRT-RTX runtime.

Only model artifacts in the caller's ignored output directory are written.
The timestep remains an input; no midpoint-only engine is disguised as arbitrary-t.
"""
import hashlib
from pathlib import Path

import torch


class ExportGraph(torch.nn.Module):
    def __init__(self, model, dtype, fields):
        super().__init__()
        self.model = model
        self.dtype = dtype
        self.fields = fields

    def forward(self, a, b, timestep):
        with torch.autocast("cuda", enabled=self.dtype != torch.float32,
                            dtype=self.dtype if self.dtype != torch.float32 else torch.float16):
            flow, mask, merged = self.model(torch.cat((a, b), 1), timestep=timestep,
                                            scale_list=[16, 8, 4, 2, 1])
            return torch.cat((flow[-1], mask), 1).float() if self.fields else merged[-1]


class RifeRuntime:
    def __init__(self, model, checkpoint, h, w, dtype, directory, fields=False):
        import tensorrt_rtx as trt
        self.trt = trt
        self.logger = trt.Logger(trt.Logger.WARNING)
        folder = Path(directory)
        folder.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256(Path(checkpoint).read_bytes()).hexdigest()[:12]
        tag = f"rife-{'fields' if fields else 'rgb'}-{digest}-{w}x{h}-{str(dtype).split('.')[-1]}-rtx{trt.__version__}"
        if dtype == torch.bfloat16:
            tag += "-deconv32"
        onnx_path, engine_path = folder / (tag + ".onnx"), folder / (tag + ".engine")
        self.timestep = torch.full((1, 1, 1, 1), 0.5, device="cuda", dtype=dtype)
        example = torch.zeros((1, 3, h, w), device="cuda", dtype=dtype)
        graph = ExportGraph(model, dtype, fields).eval()
        if not onnx_path.exists():
            print(f"Exporting {onnx_path.name}", flush=True)
            with torch.inference_mode():
                torch.onnx.export(graph, (example, example.clone(), self.timestep), str(onnx_path),
                                  input_names=["a", "b", "timestep"], output_names=["image"],
                                  opset_version=18, dynamo=False)
        if not engine_path.exists():
            print(f"Building {engine_path.name}", flush=True)
            builder = trt.Builder(self.logger)
            network = builder.create_network(0)
            parser = trt.OnnxParser(network, self.logger)
            if not parser.parse(onnx_path.read_bytes()):
                errors = "\n".join(str(parser.get_error(i)) for i in range(parser.num_errors))
                raise RuntimeError(f"TensorRT-RTX ONNX parse failed:\n{errors}")
            config = builder.create_builder_config()
            config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 1 << 30)
            serialized = builder.build_serialized_network(network, config)
            if serialized is None:
                raise RuntimeError("TensorRT-RTX engine build returned no engine")
            engine_path.write_bytes(bytes(serialized))
        self.runtime = trt.Runtime(self.logger)
        self.engine = self.runtime.deserialize_cuda_engine(engine_path.read_bytes())
        if self.engine is None:
            raise RuntimeError("TensorRT-RTX engine deserialization failed")
        self.context = self.engine.create_execution_context()
        if self.context is None:
            raise RuntimeError("TensorRT-RTX execution context creation failed")
        output_shape = tuple(self.engine.get_tensor_shape("image"))
        if any(d <= 0 for d in output_shape):
            raise RuntimeError(f"unexpected dynamic output shape {output_shape}")
        output_type = self.engine.get_tensor_dtype("image")
        types = {trt.float32: torch.float32, trt.float16: torch.float16, trt.bfloat16: torch.bfloat16}
        if output_type not in types:
            raise RuntimeError(f"unsupported output dtype {output_type}")
        self.output = torch.empty(output_shape, device="cuda", dtype=types[output_type])
        self.version = trt.__version__
        self.engine_path = str(engine_path)

    def __call__(self, a, b, timestep=0.5):
        self.timestep.fill_(timestep)
        for name, value in (("a", a), ("b", b), ("timestep", self.timestep), ("image", self.output)):
            expected_shape = tuple(self.engine.get_tensor_shape(name))
            expected_type = {self.trt.float32: torch.float32, self.trt.float16: torch.float16,
                             self.trt.bfloat16: torch.bfloat16}.get(
                self.engine.get_tensor_dtype(name))
            if not value.is_cuda or not value.is_contiguous() or tuple(value.shape) != expected_shape or value.dtype != expected_type:
                raise ValueError(f"{name}: expected contiguous CUDA {expected_type} {expected_shape}, "
                                 f"got {value.device} {value.dtype} {tuple(value.shape)}")
            if not self.context.set_tensor_address(name, value.data_ptr()):
                raise RuntimeError(f"TensorRT-RTX could not bind {name}")
        if not self.context.execute_async_v3(torch.cuda.current_stream().cuda_stream):
            raise RuntimeError("TensorRT-RTX inference failed")
        return self.output
