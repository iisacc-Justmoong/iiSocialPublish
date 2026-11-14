"""Python binding for iiSocialIntegration's synchronous native GraphQL API."""

import ctypes
import ctypes.util
import json
import os


class GraphQLClient:
    """Use from a worker: network calls are synchronous and never retry implicitly."""

    def __init__(self, library=None, *, certificate_authority=None):
        path = (os.fspath(library) if library is not None else
                os.environ.get("IISOCIALINTEGRATION_LIBRARY") or
                ctypes.util.find_library("iiSocialIntegration"))
        if not path:
            raise OSError("iiSocialIntegration native library was not found")
        self._library = ctypes.CDLL(path)
        self._library.iiSocialIntegrationGraphQLExecute.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        self._library.iiSocialIntegrationGraphQLExecute.restype = ctypes.c_void_p
        self._library.iiSocialIntegrationGraphQLFree.argtypes = [ctypes.c_void_p]
        self._library.iiSocialIntegrationGraphQLFree.restype = None
        self._certificate_authority = os.fspath(certificate_authority) if certificate_authority else ""

    def _execute(self, payload):
        if self._certificate_authority:
            payload["certificateAuthority"] = self._certificate_authority
        data = json.dumps(payload, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
        pointer = self._library.iiSocialIntegrationGraphQLExecute(data, len(data))
        if not pointer:
            raise MemoryError("GraphQL response allocation failed")
        try:
            return json.loads(ctypes.string_at(pointer))
        finally:
            self._library.iiSocialIntegrationGraphQLFree(pointer)

    @staticmethod
    def _document(query, variables, operation_name):
        return {"query": query, "variables": {} if variables is None else variables,
                "operationName": operation_name or ""}

    def execute(self, query, variables=None, *, operation_name=None, accounts=None):
        """Run a provider GraphQL document using host-owned account settings."""
        return self._execute({"mode": "provider", "document": self._document(query, variables, operation_name),
                              "accounts": {} if accounts is None else accounts})

    def execute_remote(self, endpoint, query, variables=None, *, operation_name=None, access_token=""):
        """Send the original document to a host-selected trusted GraphQL endpoint."""
        return self._execute({"mode": "remote", "document": self._document(query, variables, operation_name),
                              "endpoint": {"url": endpoint, "accessToken": access_token}})
