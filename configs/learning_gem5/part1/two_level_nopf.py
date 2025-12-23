import m5
import os

from m5.objects import (
    System, Root, Process, SEWorkload,
    SrcClockDomain, VoltageDomain,
    AddrRange, SystemXBar, L2XBar, MemCtrl, DDR3_1600_8x8,
    X86TimingSimpleCPU,
)

m5.util.addToPath("../../")
from caches import *
from common import SimpleOpts

thispath = os.path.dirname(os.path.realpath(__file__))
default_binary = os.path.join(
    thispath,
    "../../../",
    "tests/test-progs/hello/bin/x86/linux/hello",
)

SimpleOpts.add_option("binary", nargs="?", default=default_binary)
SimpleOpts.add_option("--maxinsts", type=int, default=200000)
SimpleOpts.add_option("--maxtick", type=int, default=0,
                      help="Stop after this many ticks (0 = no limit)")
SimpleOpts.add_option("--warmup-insts", type=int, default=0,
                      help="Warmup instructions before stats reset")

args = SimpleOpts.parse_args()

system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = X86TimingSimpleCPU()
system.cpu.max_insts_any_thread = args.maxinsts

system.cpu.icache = L1ICache(args)
system.cpu.dcache = L1DCache(args)
system.cpu.icache.connectCPU(system.cpu)
system.cpu.dcache.connectCPU(system.cpu)

system.l2bus = L2XBar()
system.cpu.icache.connectBus(system.l2bus)
system.cpu.dcache.connectBus(system.l2bus)

system.l2cache = L2Cache(args)
# Leave system.l2cache.prefetcher unset so it keeps the default (no prefetcher)
system.l2cache.connectCPUSideBus(system.l2bus)

system.membus = SystemXBar()
system.l2cache.connectMemSideBus(system.membus)

system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.workload = SEWorkload.init_compatible(args.binary)

process = Process()
process.cmd = [args.binary]
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

warmup = int(args.warmup_insts)
maxinsts = int(args.maxinsts)

print("Beginning simulation!")

if warmup > 0:
    print(f"Warmup: {warmup} insts")
    system.cpu.max_insts_any_thread = warmup
    m5.simulate()
    m5.stats.reset()

if maxinsts > 0:
    print(f"Measure: {maxinsts} insts")
    system.cpu.max_insts_any_thread = maxinsts
else:
    print("Measure: run until program exits")
    system.cpu.max_insts_any_thread = 0

exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
