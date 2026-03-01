#!/bin/bash
set -euo pipefail

REGION="us-east-1"
AMI_ID="ami-08c40ec9ead489470"  # Ubuntu 22.04 x86_64
INSTANCE_TYPE="c6i.large"       # Intel Xeon (Ice Lake), 2 vCPU, 4 GiB
KEY_NAME="orderbook-key"
SG_NAME="orderbook-sg"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== OrderBook EC2 Spot Benchmark ==="
echo "Instance type: $INSTANCE_TYPE"
echo "Region:        $REGION"

# 1. Ensure default VPC exists.
VPC_ID=$(aws ec2 describe-vpcs \
    --region "$REGION" \
    --query "Vpcs[?IsDefault].VpcId | [0]" \
    --output text)

if [ "$VPC_ID" == "None" ] || [ -z "$VPC_ID" ]; then
    echo "No default VPC found. Run aws_cloud_init.sh first."
    exit 1
fi
echo "VPC: $VPC_ID"

# 2. Ensure security group exists.
SG_ID=$(aws ec2 describe-security-groups \
    --filters "Name=group-name,Values=$SG_NAME" "Name=vpc-id,Values=$VPC_ID" \
    --region "$REGION" \
    --query "SecurityGroups[0].GroupId" --output text)

if [ "$SG_ID" == "None" ] || [ -z "$SG_ID" ]; then
    echo "Security group not found. Creating..."
    SG_ID=$(aws ec2 create-security-group \
        --group-name "$SG_NAME" \
        --description "OrderBook benchmark SG" \
        --vpc-id "$VPC_ID" \
        --region "$REGION" \
        --query "GroupId" --output text)

    aws ec2 authorize-security-group-ingress \
        --group-id "$SG_ID" \
        --protocol tcp --port 22 --cidr 0.0.0.0/0 \
        --region "$REGION"
fi
echo "Security Group: $SG_ID"

# 3. Ensure key pair exists.
if [ ! -f "$KEY_NAME.pem" ]; then
    echo "Key pair $KEY_NAME.pem not found. Run aws_cloud_init.sh first."
    exit 1
fi

# 4. Launch spot instance.
echo "Requesting spot instance ($INSTANCE_TYPE)..."
INSTANCE_ID=$(aws ec2 run-instances \
    --image-id "$AMI_ID" \
    --instance-type "$INSTANCE_TYPE" \
    --key-name "$KEY_NAME" \
    --security-group-ids "$SG_ID" \
    --instance-market-options '{"MarketType":"spot","SpotOptions":{"SpotInstanceType":"one-time"}}' \
    --iam-instance-profile Name=orderbook-benchmark-role \
    --user-data "file://$SCRIPT_DIR/cloud_init.sh" \
    --region "$REGION" \
    --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=orderbook-benchmark}]" \
    --query 'Instances[0].InstanceId' \
    --output text)

echo "Instance: $INSTANCE_ID"

# 5. Wait for running state.
echo "Waiting for instance to start..."
aws ec2 wait instance-running --instance-ids "$INSTANCE_ID" --region "$REGION"

PUBLIC_IP=$(aws ec2 describe-instances \
    --instance-ids "$INSTANCE_ID" \
    --region "$REGION" \
    --query 'Reservations[0].Instances[0].PublicIpAddress' \
    --output text)
echo "Running at: $PUBLIC_IP"

# 6. Fetch spot price for cost reporting.
SPOT_PRICE=$(aws ec2 describe-spot-price-history \
    --instance-types "$INSTANCE_TYPE" \
    --product-descriptions "Linux/UNIX" \
    --region "$REGION" \
    --max-items 1 \
    --query 'SpotPriceHistory[0].SpotPrice' \
    --output text 2>/dev/null || echo "unknown")

ON_DEMAND_PRICE="0.085"  # c6i.large us-east-1 on-demand
echo "Spot price: \$$SPOT_PRICE/hr (on-demand: \$$ON_DEMAND_PRICE/hr)"
if [ "$SPOT_PRICE" != "unknown" ]; then
    SAVINGS=$(echo "scale=0; (1 - $SPOT_PRICE / $ON_DEMAND_PRICE) * 100" | bc 2>/dev/null || echo "?")
    echo "Savings:    ${SAVINGS}% vs on-demand"
fi

# 7. Wait for cloud-init to finish (instance self-terminates).
echo "Waiting for benchmark to complete (instance will self-terminate)..."
aws ec2 wait instance-terminated \
    --instance-ids "$INSTANCE_ID" \
    --region "$REGION" \
    --cli-read-timeout 600 || {
    echo "Timed out. Force terminating..."
    aws ec2 terminate-instances --instance-ids "$INSTANCE_ID" --region "$REGION"
    aws ec2 wait instance-terminated --instance-ids "$INSTANCE_ID" --region "$REGION"
}

echo ""
echo "Benchmark complete. Instance terminated."

# 8. Show S3 results location.
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
BUCKET_NAME="orderbook-benchmark-${ACCOUNT_ID}"
echo "Results: aws s3 ls s3://$BUCKET_NAME/"
echo "Fetch:   aws s3 cp s3://$BUCKET_NAME/ ./results/ --recursive"
