"""Check source/flow/timestep conventions before trusting learned-model scores."""
import unittest
import torch
import torch.nn.functional as F

from learned_probe import Candidate


@unittest.skipUnless(torch.cuda.is_available(), "CUDA renderer test")
class GeometryTest(unittest.TestCase):
    def test_translation_at_three_phases_and_exact_endpoints(self):
        torch.manual_seed(123)
        a = torch.rand(1, 3, 64, 64, device="cuda")
        b = F.pad(a[:, :, :, :-4], (4, 0, 0, 0), mode="replicate")
        fields = torch.zeros(1, 5, 64, 64, device="cuda")
        fields[:, 0] = -2  # midpoint back to A
        fields[:, 2] = 2   # midpoint forward to B
        for phase, displacement in ((0.25, 1), (0.5, 2), (0.75, 3)):
            output = Candidate.render_pair(a, b, fields, phase)
            expected = a[:, :, 8:-8, 8-displacement:-8-displacement]
            self.assertLess((output[:, :, 8:-8, 8:-8] - expected).abs().max().item(), 1e-5)
        self.assertTrue(torch.equal(Candidate.render_pair(a, b, fields, 0), a))
        self.assertTrue(torch.equal(Candidate.render_pair(a, b, fields, 1), b))

    def test_static_frame_survives_nonuniform_visibility(self):
        a = torch.rand(1, 3, 32, 32, device="cuda")
        fields = torch.zeros(1, 5, 32, 32, device="cuda")
        fields[:, 4] = torch.linspace(-8, 8, 32, device="cuda")
        for phase in (0.25, 0.5, 0.75):
            output = Candidate.render_pair(a, a, fields, phase)
            self.assertLess((output - a).abs().max().item(), 1e-5)

    def test_exact_phase_fields_do_not_use_midpoint_reprojection(self):
        a = torch.rand(1, 3, 64, 64, device="cuda")
        b = F.pad(a[:, :, :, :-4], (4, 0, 0, 0), mode="replicate")
        fields = torch.zeros(1, 5, 64, 64, device="cuda")
        fields[:, 0] = -3  # direct prediction for t=0.75
        fields[:, 2] = 1
        fields[:, 4] = 1.3
        output = Candidate.render_fields(a, b, fields)
        self.assertLess((output[:, :, 8:-8, 8:-8] - a[:, :, 8:-8, 5:-11]).abs().max().item(), 1e-5)


if __name__ == "__main__":
    unittest.main()
