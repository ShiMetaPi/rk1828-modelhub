# -*- coding: utf-8 -*-
"""长期记忆封装：优先 mem0（全本地），环境不可用降级为进程内档案。

板上全本地拓扑：LLM=127.0.0.1:8081 rkllm3-server（OpenAI 兼容），
embedder=fastembed ONNX CPU（bge-small-zh-v1.5, 512 维），向量库=faiss 文件。
chat_web 只认本模块接口，不感知底层模式。
"""
import os
import re
import threading

LLM_BASE = os.environ.get("CHAT_WEB_LLM_BASE", "http://127.0.0.1:8081/v1")
LLM_MODEL = os.environ.get("CHAT_WEB_LLM_MODEL", "minicpm5-w4")
MEM_DIR = os.environ.get("CHAT_WEB_MEM_DIR", "/userdata/mem0_data")
# 板上无外网：fastembed 走预填缓存（PC 侧下好的 bge-small-zh-v1.5）。
# 此版 fastembed 不认本地路径且在线分支必先联网 model_info()——因此给
# mem0 的 fastembed 封装打补丁，强制 local_files_only=True + cache_dir。
EMBED_MODEL = os.environ.get("CHAT_WEB_EMBED_MODEL", "BAAI/bge-small-zh-v1.5")
EMBED_CACHE = os.environ.get("CHAT_WEB_EMBED_CACHE",
                             "/userdata/mem0_offline/fastembed_cache")
EMBED_DIMS = 512
USER_ID = "demo-user"


def _patch_fastembed_offline():
    """mem0 的 FastEmbedEmbedding 只传 model_name；这里注入离线加载参数。"""
    import mem0.embeddings.fastembed as mfe
    real = mfe.TextEmbedding

    class _OfflineTE(real):
        def __init__(self, model_name=EMBED_MODEL, **kw):
            kw.setdefault("cache_dir", EMBED_CACHE)
            kw.setdefault("local_files_only", True)
            super().__init__(model_name, **kw)

    mfe.TextEmbedding = _OfflineTE

# ---- 规则抽取（降级模式与 mem0 抽取失败时的兜底共用）----
NAME_RE = re.compile(
    r"(?:我(?:的名字)?(?:叫|是)|My name is|call me)\s*[:：]?\s*"
    r"([一-龥A-Za-z0-9_\-]{1,12})")
STOPWORDS = {"什么", "谁", "你", "他", "她", "一个", "中国人", "机器人", "AI", "ai"}


def extract_facts(text):
    """从用户消息抽稳定事实。当前只抽名字（YAGNI：别的等需求）。"""
    facts = []
    for m in NAME_RE.finditer(text):
        name = m.group(1).strip()
        if name and name not in STOPWORDS:
            facts.append("用户名字：%s" % name)
    return facts


def compose_system(memory_facts, recap):
    """组 system 消息：记忆档案在前（常驻顶部），摘要在后。空则返回空串。"""
    parts = []
    if memory_facts:
        parts.append("已知用户信息（长期记忆，回答相关问题时使用）：" +
                     "；".join(memory_facts))
    if recap:
        parts.append(recap)
    return "\n".join(parts)


class MemoryStore:
    """mem0 优先；失败降级进程内档案（名字级记忆），接口不变。"""

    def __init__(self):
        self._lock = threading.Lock()
        self._profile = {}          # 降级档案 {fact_str: True}
        self._mem = None
        self.mode = "off"
        if os.environ.get("CHAT_WEB_MEMORY", "1") != "1":
            return
        try:
            os.environ.setdefault("MEM0_TELEMETRY", "False")
            _patch_fastembed_offline()
            from mem0 import Memory
            cfg = {
                "llm": {"provider": "openai", "config": {
                    "model": LLM_MODEL, "openai_base_url": LLM_BASE,
                    "api_key": "none", "temperature": 0.1,
                    "max_tokens": 400}},
                "embedder": {"provider": "fastembed", "config": {
                    "model": EMBED_MODEL}},
                "vector_store": {"provider": "faiss", "config": {
                    "path": MEM_DIR, "collection_name": "chat_web",
                    "embedding_model_dims": EMBED_DIMS}},
                "disable_history": True,
                "custom_fact_extraction_prompt": (
                    "从下面的对话中提取关于用户的基本事实（名字、偏好、"
                    "重要信息），每条一行，最多5条，没有就输出空。"
                    "只输出事实本身，不要解释。\n对话："),
            }
            self._mem = Memory.from_config(cfg)
            self.mode = "mem0"
        except Exception as e:
            print("[memory] mem0 不可用，降级档案模式：%s" % e, flush=True)
            self.mode = "profile"

    def recall(self, query):
        """命中记忆（同步，chat_web 每次生成前调用）。绝不抛。"""
        try:
            if self._mem is not None:
                res = self._mem.search(query, filters={"user_id": USER_ID},
                                       limit=3)
                hits = res.get("results") if isinstance(res, dict) else res
                return [h["memory"] for h in (hits or []) if h.get("memory")]
            with self._lock:
                return list(self._profile.keys())
        except Exception as e:
            print("[memory] recall 失败：%s" % e, flush=True)
            return []

    def add_turn(self, q, a):
        """记录一轮（调用方放后台线程）。

        抽取用轻量正则（mem0 官方抽取 prompt 达 7831 token，2B 模型跑不了），
        抽到的事实以 infer=False 直接入 faiss——mem0 只当向量检索库，不调 LLM。
        """
        try:
            facts = extract_facts(q)
            for f in facts:
                with self._lock:
                    self._profile[f] = True
            if self._mem is not None and facts:
                for f in facts:
                    self._mem.add(
                        [{"role": "user", "content": f}],
                        user_id=USER_ID, infer=False)
        except Exception as e:
            print("[memory] add_turn 失败（忽略）：%s" % e, flush=True)

    def reset(self):
        try:
            with self._lock:
                self._profile.clear()
            if self._mem is not None:
                self._mem.reset()
        except Exception as e:
            print("[memory] reset 失败：%s" % e, flush=True)
