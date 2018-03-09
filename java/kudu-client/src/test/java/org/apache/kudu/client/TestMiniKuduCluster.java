/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. See accompanying LICENSE file.
 */
package org.apache.kudu.client;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.IOException;
import java.net.Socket;

import org.apache.kudu.client.KuduClient.KuduClientBuilder;
import org.junit.Test;

import com.google.common.net.HostAndPort;

public class TestMiniKuduCluster {

  private static final int NUM_TABLET_SERVERS = 3;
  private static final int NUM_MASTERS = 1;
  private static final long SLEEP_TIME_MS = 10000;

  @Test(timeout = 50000)
  public void test() throws Exception {
    try (MiniKuduCluster cluster = new MiniKuduCluster.MiniKuduClusterBuilder()
                                                      .numMasters(NUM_MASTERS)
                                                      .numTservers(NUM_TABLET_SERVERS)
                                                      .build()) {
      assertEquals(NUM_MASTERS, cluster.getMasterHostPorts().size());
      assertEquals(NUM_TABLET_SERVERS, cluster.getTserverHostPorts().size());

      {
        // Kill the master.
        HostAndPort masterHostPort = cluster.getMasterHostPorts().get(0);
        testHostPort(masterHostPort, true);
        cluster.killMasterOnHostPort(masterHostPort);

        testHostPort(masterHostPort, false);

        // Restart the master.
        cluster.restartDeadMasterOnHostPort(masterHostPort);

        // Test we can reach it.
        testHostPort(masterHostPort, true);
      }

      {
        // Kill the first TS.
        HostAndPort tsHostPort = cluster.getTserverHostPorts().get(0);
        testHostPort(tsHostPort, true);
        cluster.killTabletServerOnHostPort(tsHostPort);

        testHostPort(tsHostPort, false);

        // Restart it.
        cluster.restartDeadTabletServerOnHostPort(tsHostPort);

        testHostPort(tsHostPort, true);
      }
    }
  }

  @Test(timeout = 50000)
  public void testKerberos() throws Exception {
    FakeDNS.getInstance().install();
    try (MiniKuduCluster cluster = new MiniKuduCluster.MiniKuduClusterBuilder()
                                                      .numMasters(NUM_MASTERS)
                                                      .numTservers(NUM_TABLET_SERVERS)
                                                      .enableKerberos()
                                                      .build()) {
      KuduClient client = new KuduClientBuilder(cluster.getMasterAddresses()).build();
      ListTablesResponse resp = client.getTablesList();
      assertTrue(resp.getTablesList().isEmpty());
    }
  }

  /**
   * Test whether the specified host and port is open or closed, waiting up to a certain time.
   * @param hp the host and port to test
   * @param testIsOpen true if we should want it to be open, false if we want it closed
   */
  private static void testHostPort(HostAndPort hp,
      boolean testIsOpen) throws InterruptedException {
    DeadlineTracker tracker = new DeadlineTracker();
    while (tracker.getElapsedMillis() < SLEEP_TIME_MS) {
      try {
        Socket socket = new Socket(hp.getHost(), hp.getPort());
        socket.close();
        if (testIsOpen) {
          return;
        }
      } catch (IOException e) {
        if (!testIsOpen) {
          return;
        }
      }
      Thread.sleep(200);
    }
    fail("HostAndPort " + hp + " is still " + (testIsOpen ? "closed " : "open"));
  }
}
