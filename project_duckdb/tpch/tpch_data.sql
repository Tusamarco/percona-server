-- Copyright (c) 2026-present, Marco Tusa. All rights reserved.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
--
-- TPC-H-like data for a quick check of the 22 queries, about scale factor
-- 0.01 (lineitem ~60,000 rows), deterministic (CRC32 of the row number). The
-- value domains follow the TPC-H specification (types, containers, segments,
-- ship modes, priorities, nations and regions, phone country codes, comment
-- words the queries look for), so every query finds rows. Not a TPC-H
-- dbgen replacement: use HammerDB or dbgen for real runs.
USE tpch;
SET SESSION foreign_key_checks = 0;   -- as HammerDB's loader
SET SESSION cte_max_recursion_depth = 100000;
DROP TABLE IF EXISTS gen_n;
CREATE TABLE gen_n (n INT PRIMARY KEY);
INSERT INTO gen_n WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 60000) SELECT n FROM s;

INSERT INTO REGION VALUES (0, 'AFRICA', 'lar deposits'), (1, 'AMERICA', 'hs use ironic requests'),
  (2, 'ASIA', 'ges. thinly even pinto beans'), (3, 'EUROPE', 'ly final courts cajole'),
  (4, 'MIDDLE EAST', 'uickly special accounts');
INSERT INTO NATION VALUES (0,'ALGERIA',0,'c'),(1,'ARGENTINA',1,'c'),(2,'BRAZIL',1,'c'),(3,'CANADA',1,'c'),
  (4,'EGYPT',4,'c'),(5,'ETHIOPIA',0,'c'),(6,'FRANCE',3,'c'),(7,'GERMANY',3,'c'),(8,'INDIA',2,'c'),
  (9,'INDONESIA',2,'c'),(10,'IRAN',4,'c'),(11,'IRAQ',4,'c'),(12,'JAPAN',2,'c'),(13,'JORDAN',4,'c'),
  (14,'KENYA',0,'c'),(15,'MOROCCO',0,'c'),(16,'MOZAMBIQUE',0,'c'),(17,'PERU',1,'c'),(18,'CHINA',2,'c'),
  (19,'ROMANIA',3,'c'),(20,'SAUDI ARABIA',4,'c'),(21,'VIETNAM',2,'c'),(22,'RUSSIA',3,'c'),
  (23,'UNITED KINGDOM',3,'c'),(24,'UNITED STATES',1,'c');

-- 100 suppliers; every 10th has "Customer Complaints" in its comment (Q16)
INSERT INTO SUPPLIER SELECT n, CRC32(CONCAT('sn', n)) % 25,
  IF(n % 10 = 0, 'slyly Customer pending Complaints', 'carefully regular packages'),
  CONCAT('Supplier#', LPAD(n, 9, '0')), CONCAT('addr', n),
  CONCAT(CRC32(CONCAT('sn', n)) % 25 + 10, '-', LPAD(n % 1000, 3, '0'), '-555-0100'),
  (CRC32(CONCAT('sb', n)) % 1099998) / 100 - 999.99
  FROM gen_n WHERE n <= 100;

-- 1,500 customers; phone prefix = nation key + 10 (Q22)
INSERT INTO CUSTOMER SELECT n,
  ELT(CRC32(CONCAT('cm', n)) % 5 + 1, 'AUTOMOBILE', 'BUILDING', 'FURNITURE', 'MACHINERY', 'HOUSEHOLD'),
  CRC32(CONCAT('cn', n)) % 25, CONCAT('Customer#', LPAD(n, 9, '0')), CONCAT('caddr', n),
  CONCAT(CRC32(CONCAT('cn', n)) % 25 + 10, '-', LPAD(n % 1000, 3, '0'), '-555-0199'),
  (CRC32(CONCAT('cb', n)) % 1099998) / 100 - 999.99, 'quickly even deposits'
  FROM gen_n WHERE n <= 1500;

-- 2,000 parts: type = S1 S2 S3, container = S1 S2, brand Brand#MN, a colour in the name
INSERT INTO PART SELECT n,
  CONCAT(ELT(CRC32(CONCAT('t1', n)) % 6 + 1, 'STANDARD', 'SMALL', 'MEDIUM', 'LARGE', 'ECONOMY', 'PROMO'), ' ',
         ELT(CRC32(CONCAT('t2', n)) % 5 + 1, 'ANODIZED', 'BURNISHED', 'PLATED', 'POLISHED', 'BRUSHED'), ' ',
         ELT(CRC32(CONCAT('t3', n)) % 5 + 1, 'TIN', 'NICKEL', 'BRASS', 'STEEL', 'COPPER')),
  CRC32(CONCAT('sz', n)) % 50 + 1,
  CONCAT('Brand#', CRC32(CONCAT('m', n)) % 5 + 1, CRC32(CONCAT('b', n)) % 5 + 1),
  CONCAT(ELT(CRC32(CONCAT('c1', n)) % 8 + 1, 'almond', 'forest', 'green', 'blush', 'navy', 'khaki', 'linen', 'tan'),
         ' ', ELT(CRC32(CONCAT('c2', n)) % 4 + 1, 'antique', 'beige', 'bisque', 'black')),
  CONCAT(ELT(CRC32(CONCAT('k1', n)) % 5 + 1, 'SM', 'LG', 'MED', 'JUMBO', 'WRAP'), ' ',
         ELT(CRC32(CONCAT('k2', n)) % 8 + 1, 'CASE', 'BOX', 'BAG', 'JAR', 'PKG', 'PACK', 'CAN', 'DRUM')),
  CONCAT('Manufacturer#', CRC32(CONCAT('m', n)) % 5 + 1), 900 + (n % 200) + (n % 1000) / 100, 'final deposits'
  FROM gen_n WHERE n <= 2000;

-- 4 suppliers per part
INSERT INTO PARTSUPP SELECT p.n, (p.n + k.n * 25) % 100 + 1, CRC32(CONCAT('sc', p.n, k.n)) % 1000 + 1,
  CRC32(CONCAT('aq', p.n, k.n)) % 9999 + 1, 'blithely even requests'
  FROM gen_n p JOIN gen_n k ON k.n <= 4 WHERE p.n <= 2000;

-- 15,000 orders, 1992-01-01 .. 1998-08-02; some comments with "special requests" (Q13)
INSERT INTO ORDERS SELECT DATE '1992-01-01' + INTERVAL (CRC32(CONCAT('od', n)) % 2405) DAY, n,
  CRC32(CONCAT('oc', n)) % 1500 + 1,
  ELT(CRC32(CONCAT('op', n)) % 5 + 1, '1-URGENT', '2-HIGH', '3-MEDIUM', '4-NOT SPECIFIED', '5-LOW'), 0,
  CONCAT('Clerk#', LPAD(n % 1000, 9, '0')), 'O', 0,
  IF(n % 17 = 0, 'pending special theodolites requests', 'furiously regular accounts')
  FROM gen_n WHERE n <= 15000;

-- 1..7 lines per order (~60,000 rows)
INSERT INTO LINEITEM
SELECT o.O_ORDERDATE + INTERVAL (CRC32(CONCAT('sd', o.O_ORDERKEY, k.n)) % 121 + 1) DAY,
  o.O_ORDERKEY, (CRC32(CONCAT('di', o.O_ORDERKEY, k.n)) % 11) / 100,
  (CRC32(CONCAT('qt', o.O_ORDERKEY, k.n)) % 50 + 1) * (900 + (CRC32(CONCAT('pk', o.O_ORDERKEY, k.n)) % 2000) % 200),
  (CRC32(CONCAT('pk', o.O_ORDERKEY, k.n)) % 2000 + 1 + (CRC32(CONCAT('sp', o.O_ORDERKEY, k.n)) % 4) * 25) % 100 + 1,
  CRC32(CONCAT('qt', o.O_ORDERKEY, k.n)) % 50 + 1,
  IF(o.O_ORDERDATE + INTERVAL (CRC32(CONCAT('sd', o.O_ORDERKEY, k.n)) % 121 + 31) DAY <= DATE '1995-06-17',
     ELT(CRC32(CONCAT('rf', o.O_ORDERKEY, k.n)) % 2 + 1, 'R', 'A'), 'N'),
  CRC32(CONCAT('pk', o.O_ORDERKEY, k.n)) % 2000 + 1,
  IF(o.O_ORDERDATE + INTERVAL (CRC32(CONCAT('sd', o.O_ORDERKEY, k.n)) % 121 + 1) DAY > DATE '1995-06-17', 'O', 'F'),
  (CRC32(CONCAT('tx', o.O_ORDERKEY, k.n)) % 9) / 100,
  o.O_ORDERDATE + INTERVAL (CRC32(CONCAT('cd', o.O_ORDERKEY, k.n)) % 61 + 30) DAY,
  o.O_ORDERDATE + INTERVAL (CRC32(CONCAT('sd', o.O_ORDERKEY, k.n)) % 121 + 1 + CRC32(CONCAT('rd', o.O_ORDERKEY, k.n)) % 30 + 1) DAY,
  ELT(CRC32(CONCAT('sm', o.O_ORDERKEY, k.n)) % 7 + 1, 'REG AIR', 'AIR', 'RAIL', 'SHIP', 'TRUCK', 'MAIL', 'FOB'),
  k.n, ELT(CRC32(CONCAT('si', o.O_ORDERKEY, k.n)) % 4 + 1, 'DELIVER IN PERSON', 'COLLECT COD', 'NONE', 'TAKE BACK RETURN'),
  'ironic deposits'
FROM ORDERS o JOIN gen_n k ON k.n <= CRC32(CONCAT('ln', o.O_ORDERKEY)) % 7 + 1;

-- order status and total from the lines, as dbgen derives them
UPDATE ORDERS o JOIN (SELECT L_ORDERKEY, SUM(L_EXTENDEDPRICE * (1 + L_TAX) * (1 - L_DISCOUNT)) AS total,
    SUM(L_LINESTATUS = 'O') AS open_lines, COUNT(*) AS lines_count FROM LINEITEM GROUP BY L_ORDERKEY) l
  ON l.L_ORDERKEY = o.O_ORDERKEY
  SET o.O_TOTALPRICE = l.total,
      o.O_ORDERSTATUS = IF(l.open_lines = l.lines_count, 'O', IF(l.open_lines = 0, 'F', 'P'));
DROP TABLE gen_n;
ANALYZE TABLE REGION, NATION, SUPPLIER, CUSTOMER, PART, PARTSUPP, ORDERS, LINEITEM;

ALTER TABLE REGION SECONDARY_LOAD;
ALTER TABLE NATION SECONDARY_LOAD;
ALTER TABLE SUPPLIER SECONDARY_LOAD;
ALTER TABLE CUSTOMER SECONDARY_LOAD;
ALTER TABLE PART SECONDARY_LOAD;
ALTER TABLE PARTSUPP SECONDARY_LOAD;
ALTER TABLE ORDERS SECONDARY_LOAD;
ALTER TABLE LINEITEM SECONDARY_LOAD;
-- Q15 (HammerDB creates and drops it around the query)
CREATE OR REPLACE VIEW revenue0 (supplier_no, total_revenue) AS
  SELECT l_suppkey, SUM(l_extendedprice * (1 - l_discount)) FROM LINEITEM
   WHERE l_shipdate >= DATE '1996-01-01' AND l_shipdate < DATE '1996-01-01' + INTERVAL '3' MONTH
   GROUP BY l_suppkey;
SELECT 'LINEITEM' AS t, COUNT(*) FROM LINEITEM UNION ALL SELECT 'ORDERS', COUNT(*) FROM ORDERS;
